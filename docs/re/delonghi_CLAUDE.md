# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

Firmware that reverse-engineers and ultimately emulates a BLE remote control for a De'Longhi
mobile air-conditioner. The remote is a BLE peripheral named **`Ganymede`**, address
**`00:A0:50:XX:XX:XX`**, advertising as a HID keyboard (appearance `0x03C1` / 961).

Phased goal, in order:
1. **Done as evidence:** extract the HID command path and button reports from Android HCI snoop logs.
2. **Current milestone:** ESP32-S3 emulates the remote as a BLE HID peripheral to drive the A/C.
3. Later: integrate with Home Assistant for remote control.

`SPECS.md` is the historical probe spec. The current emulator/refactor plan is
`docs/esp-idf-factorization-plan.md`.

## The central technical problem (read this first)

Android (nRF Connect) ordinary GATT views were blocked from HID fields with
`BLUETOOTH_PRIVILEGED`, but Android HCI snoop captures later exposed the HID
Report Map and button reports. The useful path is now known:

- `0x2A4B` Report Map is a standard boot-keyboard map with one 8-byte input report.
- `0x2A4D` input Report notifications on value handle `0x003b` carry button reports.
- There is no Report ID, no observed key-release report, and reports are duplicated on air.

For the emulator, every design decision serves BLE HID peripheral fidelity and reliable pairing with
the A/C central.

## Source of truth

This repo contains a standalone ESP-IDF/NimBLE firmware with two roles selected at build time:
the old central probe in `main/main.c`, and the current peripheral emulator in `main/emulator.c`.
Read these documents before changing behavior:

- **`docs/esp-idf-factorization-plan.md`** — current source of truth for the emulator, evidence order,
  manual findings, cleanup policy, and desired module split.
- **`SPECS.md`** — historical probe specification: BLE central requirements, JSON log schema,
  characteristic decoders, and safety constraints. Useful for preserving the probe role.
- **`ESP32-S3-SuperMini-BOARD.md`** — ESP32-S3 SuperMini hardware/GPIO/boot rules for the real board.
- **`logs/`** — see `logs/README.md` for the retained evidence set. Treat the btsnoop captures as the
  reference for report bytes and pairing behavior; do not invent characteristic behavior not seen here.

## Build / flash / monitor

Toolchain is **standalone ESP-IDF**. BLE stack is **NimBLE** (ESP-IDF host). Do **not**
use Arduino BLE libraries.

Target board is the **ESP32-S3 SuperMini** (target `esp32s3`). Use:

```bash
source tools/idf-env.sh
idf.py set-target esp32s3
idf.py -DAPP=emulator build   # current AC pairing target
idf.py -DAPP=probe build      # old BLE central probe
idf.py -DAPP=emulator build   # restore build cache after probe testing
idf.py flash monitor
```

Recovery if the native USB port disappears after a bad flash: hold BOOT, tap RST,
release BOOT, reflash.

> Git note: at session start the `.git` directory is empty/non-functional — `git` commands fail with
> "not a git repository". Confirm repo state before attempting commits.

## ESP32-S3 SuperMini hardware reference

Specs (source: https://www.espboards.dev/esp32/esp32-s3-super-mini/):
- ESP32-S3, dual-core Xtensa LX7 @ 240 MHz, **4 MB flash (QIO)**, **no PSRAM**, 512 KB SRAM. WiFi + BLE 5.0.
- USB-C with **native USB-Serial/JTAG** — this is the flash/console path. On the S3 SoC, native USB is
  fixed to **GPIO19 (D−)** and **GPIO20 (D+)**; never repurpose them if USB is in use.
- **LED on GPIO48** — both a red LED and a shared WS2812 RGB (cannot be used independently). A separate
  blue LED is a non-programmable battery-charge indicator. Verify polarity on the actual unit.
- **Strapping pins: GPIO0, GPIO3, GPIO45, GPIO46** — do not let external peripherals force their level at
  reset.
- Generally-safe GPIOs: 1, 2, 4, 5, 6, 7, 8, 15, 16, 17, 18, 21. ADC on A0–A5. Default labeled buses:
  I2C SDA=GPIO8 / SCL=GPIO9, SPI MOSI=6 / MISO=5 / SCK=4 / SS=7 (reassign as the design needs).
- Verify the real flash size with `esptool.py flash_id` rather than assuming 4 MB.

For the current BLE emulator no external wiring is needed — the only hardware
touchpoint is the optional status LED on GPIO48. Later display/sensor work should
centralize pins in a board header first.

## Conventions that matter here

- **Probe behavior is read-only + notification subscription.** The probe must not spam writes to the
  real remote. The only probe writes are CCCD `0x2902`, and Protocol Mode (`0x2A4E`) / HID Control
  Point (`0x2A4C`) only when explicitly requested via the serial CLI (`SPECS.md` §16).
- **Emulator behavior sends only explicit button reports.** The emulator should not emit reports until
  the user sends `press <button>` over the serial CLI.
- **Logging is line-delimited JSON over USB serial**, one object per event (`type`: `scan`, `connect`,
  `service`, `char`, `read`, `subscribe`, `notification`, …). A human-readable debug mode may exist but
  JSON is the default. See `SPECS.md` §9 for the exact shapes.
- Preserve the central probe because it is still useful for future unknowns, but do not let old probe
  assumptions override the captured emulator behavior in `docs/esp-idf-factorization-plan.md`.
- **CCCD values**: notify `01 00`, indicate `02 00`, both `03 00`, disable `00 00`.

## Device GATT quick reference (from logs)

Services: `0x1800` Generic Access, `0x1801` Generic Attribute, `0x180A` Device Information,
`0x180F` Battery, `0x181A` Environmental Sensing, `0x1812` HID.

Confirmed-readable values & decoders:
- `0x2A00` Device Name → `"Ganymede"`; `0x2A01` Appearance → `0x03C1` (Keyboard HID), uint16 LE.
- `0x2A04` Pref. Conn. Params → `80 0C 80 0C 00 00 B8 0B` = interval 4000 ms (very slow), latency 0,
  supervision multiplier 3000.
- `0x2A19` Battery → uint8 %; `0x2A6E` Temp → sint16 LE /100 °C; `0x2A6F` Humidity → uint16 LE /100 %;
  `0x2A6D` Pressure → uint32 LE /10 Pa.
- `0x2A4E` Protocol Mode reads `01` (Report mode); `0x2A22` Boot Keyboard Input reads all-zero idle.
- Device Information strings (`0x2A24`/`0x2A25`/`0x2A26`/`0x2A27`/`0x2A29`) read **0 bytes**;
  `0x2A50` PnP ID reports Ericsson AB / product 0 — likely placeholder, not meaningful.

Temperature & humidity notifications work and arrive together. The `0x2A4D`
semantics are now captured; keep the mapping in `docs/esp-idf-factorization-plan.md`
and `docs/android-capture-findings.md` synchronized when it changes.
