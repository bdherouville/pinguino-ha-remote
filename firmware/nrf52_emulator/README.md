# Phase 2 — nRF52840 emulator (the connected remote's BLE radio)

BLE **peripheral** that impersonates the Ganymede remote so the **AC connects to it**
and receives the same HID button reports. Replaces the ESP32 emulator (whose radio
the AC never saw).

**Status (2026-06-02):** `src/nrf52_emulator.ino` written and **compiles clean** under
PlatformIO (env `adafruit_feather_nrf52840`, Adafruit Bluefruit core — RAM 6.6 %, Flash
16.6 %). Full protocol baked in (advertising ADV+SCAN_RSP, GATT HID/Battery/Env, 61-byte
Report Map, 9 buttons, SMP Just Works). **Flash when the nRF52840 boards arrive.** Three
Bluefruit spots remain to confirm **on-air** (not compile blockers): SMP IO-caps default,
`addManufacturerData` payload framing, and UUID complete/incomplete flag — verify with the
nRF Sniffer.

## Hardware / stack
- **nRF52840 Pro Micro #2**, **Arduino + Adafruit nRF52 BSP (Bluefruit)**, flash UF2.
- *If Phase 1 shows the AC requires MITM + LE Secure Connections with key
  distribution Bluefruit can't express → escalate to nRF Connect SDK / Zephyr.
  Decision locked after the capture.*

## Source of truth for the port
`reference/esp-idf-nimble/main/emulator.c` (the working-logic delonghi NimBLE
emulator — **not buildable on nRF as-is**; port its behaviour). See
`docs/ganymede_protocol.md` for every value.

## Port checklist
- [ ] Advertising — **match the real PDU split** (confirmed via tshark):
      **ADV_IND** = Flags `06` + name `Ganymede` + UUID16 `180A/180F/181A` + appearance
      `0x03C1` (no mfg); **SCAN_RSP** = **Cypress mfg `ff 31 01 3b 04`** (Company `0x0131`,
      payload `3b 04`). Fast 20–40 ms, own address. *(The reference `emulator.c` puts the
      mfg in the primary ADV — move it to the scan response.)*
- [ ] GATT: HID `0x1812` (Report Map `2A4B`, Report `2A4D` R+Notify + CCCD + Report
      Ref `2908`=`00 01`, Info `2A4A`=`11 01 00 02`, Ctrl `2A4C`, Proto Mode
      `2A4E`=`01`, Boot KB `2A22`/`2A32`), Battery `180F`, Env Sensing `181A`.
- [ ] **Report Map** = the 61 bytes (verbatim in protocol doc / emulator.c).
- [ ] **Buttons** → 8-byte report `{0,0,b2,b3,0,0,0,0}`, notify **2×**, no release;
      table power/down/up/mode/eco/timer/fan/silent/flap.
- [ ] **SMP per Phase 1** (Just Works *or* MITM+SC + passkey/numcmp handler).
- [ ] CLI over USB serial: `press <button>`, `status`, `adv`, `clear-bonds`.
- [ ] (Phase 3) UART command input from the ESP32 bridge.

## Build / flash (PlatformIO — see `platformio.ini`)
```bash
pio run                       # compile (env adafruit_feather_nrf52840)
pio run -t upload             # flash via UF2 (double-tap RESET first if needed)
pio device monitor -b 115200  # serial CLI
```
Board note: the official `nordicnrf52` platform has no `nicenano` id, so we build against
`adafruit_feather_nrf52840` (same nRF52840 + Bluefruit core). The UF2 runs on the generic
Pro Micro because this firmware uses only the radio. For an exact Pro Micro pinout later,
add a custom board JSON (à la ZMK `nice_nano`).

## Verify (the project `/goal`)
With the **nRF Sniffer (#1) running in parallel**: the AC enters pairing, **connects**
to this nRF, **bonds**, subscribes to the HID Report, and `press power` **changes the
AC state**. Pre-test against the Linux central harness (`tools/linux-ble/`,
`probe_ganymede.py` / `pair_dbus.py`) before the real AC.
