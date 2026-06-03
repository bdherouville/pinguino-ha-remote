# ESP-IDF Factorization Plan

Status: 2026-06-01. Verified with ESP-IDF 6.0.1 for `esp32s3`:
default emulator build passes, `-DAPP=probe` build passes, and the build
directory has been reset to `-DAPP=emulator`.

This repo is now past the initial BLE probe phase. The useful path is known:
the CST remote is a BLE HID keyboard peripheral named `Ganymede`, and the AC
acts as the BLE central/HID host. ESP32-S3 should emulate the peripheral and
notify the same 8-byte HID reports on the input Report characteristic.

## Evidence, Newest To Oldest

1. `logs/ac-btsnoop.log` / `logs/bugreport-ac.zip`
   - AC-facing pairing attempts from the tablet/nRF Connect clone.
   - The AC initiates SMP pairing; captured requests include bonding plus MITM
     plus Secure Connections in successful/near-successful emulator-style paths.
   - This supports keeping the emulator security setting
     `sm_bonding=1`, `sm_sc=1`, `sm_mitm=1`, `BLE_HS_IO_NO_INPUT_OUTPUT`.
   - The accepted clone used the clone profile/name over the tablet address
     (`00:A0:50:XX:XX:XX` appears in Android logs), so ESP32 should advertise
     as `Ganymede` with its own address by default. MAC cloning remains a
     diagnostic option only.

2. `logs/Log 2026-05-31 22_36_25.txt`
   - Fresh nRF Connect discovery of the real remote.
   - Confirms services: GAP, GATT, Device Information, Battery,
     Environmental Sensing, HID.
   - Confirms HID service shape: Report Map, notify/read/write input Report,
     HID Information, Control Point, Protocol Mode, Boot Keyboard Input,
     Boot Keyboard Output, and a second read/write Report.

3. `logs/ganymede-btsnoop4.log`
   - Complete labeled button mapping via repeated press counts.
   - Notifications are on HID Report value handle `0x003b`.
   - Reports are duplicated on air and no release report was observed.

4. `logs/ganymede-btsnoop3.log`
   - First successful HID capture.
   - Report Map read from value handle `0x0039`, with blob reads at offsets
     22 and 44:
     `05 01 09 06 a1 01 05 07 19 e0 29 e7 15 00 25 01 75 01 95 08 81 02 95 01 75 08 81 01 95 05 75 01 05 08 19 01 29 05 91 02 95 01 75 03 91 01 95 06 75 08 15 00 25 65 05 07 19 00 29 65 81 00 c0`
   - This is a standard boot keyboard report: 8-byte input report, no Report ID.

5. Older ESP32/C6 and Android logs
   - Useful mostly as failed discovery/pairing history.
   - They should not drive the emulator design now that the HID report map and
     AC-side pairing evidence exist.

## Manual Check

`manual.pdf` is for De'Longhi PAC EL112 CST WIFI. The CST section confirms:

- The remote uses Bluetooth Low Energy with the Pinguino AC.
- Re-pairing flow: put the AC in standby, hold remote `MODE` (`D6`) for 10 s
  until LED `D10` blinks, then hold AC panel increase key `C4` for 10 s until
  two beeps. Pairing must complete within 60 s.
- Button labels used in the HID map:
  `D1` Power, `D2` Silent, `D3` airflow/fan, `D4` down, `D5` timer, `D6` mode,
  `D7` up, `D8` myEcoRealFeel, `D9` swing/flap.

## Remote Behavior To Emulate

Button reports:

| Command | Manual | Report |
| --- | --- | --- |
| `power` | D1 | `00 00 01 00 00 00 00 00` |
| `down` | D4 | `00 00 02 00 00 00 00 00` |
| `up` | D7 | `00 00 04 00 00 00 00 00` |
| `mode` | D6 | `00 00 08 00 00 00 00 00` |
| `eco` | D8 | `00 00 10 00 00 00 00 00` |
| `timer` | D5 | `00 00 20 00 00 00 00 00` |
| `fan` | D3 | `00 00 40 00 00 00 00 00` |
| `silent` | D2 | `00 00 80 00 00 00 00 00` |
| `flap` | D9 | `00 00 00 01 00 00 00 00` |

Operational assumptions:

- Advertise as connectable undirected `Ganymede`, appearance `0x03c1`.
- Prefer own ESP32 public address for fresh AC bonds. Enable `CLONE_MAC` only
  if testing a stale-identity/cache hypothesis.
- Accept AC-initiated pairing; keep MITM bit set despite NoInputNoOutput,
  because the AC-side captures show that pairing path.
- Send two identical notifications per logical press on the input Report
  characteristic and do not send a zero release report unless a later capture
  proves it is needed.

## Factorization Target

The current firmware has the right pieces but too much is concentrated in
`main.c` and `emulator.c`. Split by role and protocol boundary:

```text
main/
  app_main.c              # selects probe or emulator app
  app_config.h            # build-time role flags and board constants
  board_s3_supermini.h    # LED, USB, future I2C pins
  log_json.c/.h
  ble_addr.c/.h           # address parse/format/clone helpers
  ble_security.c/.h       # NimBLE SMP config, bond clearing, event logging
  ble_gap_common.c/.h     # common GAP event helpers

  probe_app.c/.h          # central/client app entry
  probe_scan.c/.h
  probe_client.c/.h       # service/char/descriptor discovery and reads
  gatt_decode.c/.h

  emu_app.c/.h            # peripheral/server app entry
  ganymede_profile.c/.h   # captured constants, report map, button map
  emu_adv.c/.h            # exact advertising payload
  emu_gatt.c/.h           # GATT service table and access callbacks
  emu_reports.c/.h        # report notification API and repeat policy
  env_source.c/.h
  display.c/.h
  cli.c/.h                # role-specific commands
```

Keep `probe_*` and `emu_*` independent. Shared code should be limited to logging,
address helpers, security setup, and small GATT decoders. The probe is evidence
collection; the emulator is product behavior.

## GATT Emulation Gaps To Close

Priority order:

1. Keep the existing HID input report path stable.
   - `0x2A4B` Report Map as captured.
   - `0x2A4D` input Report, read + notify, `0x2908 = 00 01`.
   - `0x2A4A` HID Information.
   - `0x2A4C` Control Point, `0x2A4E` Protocol Mode.
   - `0x2A22` Boot Keyboard Input, read + notify.

2. Add the missing HID output side for closer fidelity.
   - Boot Keyboard Output Report `0x2A32`, read/write/write-no-response.
   - Second `0x2A4D` output Report, read/write/write-no-response,
     `0x2908 = 00 02`.
   - Store LED/output bytes even if unused.

3. Add Device Information service stubs.
   - Empty manufacturer/model/serial/firmware/hardware strings, as observed.
   - PnP ID matching the capture if needed for host compatibility.

4. Fill Environmental Sensing descriptors.
   - `0x2A7D` Descriptor Value Changed indication.
   - For humidity/temp/pressure: `0x290C`, `0x290D`, `0x2901`, `0x2906`,
     plus CCCD.
   - Keep values backed by `env_source` for later BME280 integration.

5. Match GAP details.
   - Device Name `Ganymede`.
   - Appearance `0x03c1`.
   - Preferred connection parameters equivalent to the real
     `80 0c 80 0c 00 00 b8 0b` if the AC reacts to it.

Exact handle matching should remain a second-stage option. With a fresh ESP32
address the AC should discover services normally; exact handles matter mainly
when cloning an identity that the AC has already cached.

## BLE Layer Evidence

Latest live checks on 2026-06-01 narrow the failure below the GATT database:

- Android nRF Connect can connect immediately to the ESP32 emulator, so the ESP32
  NimBLE peripheral path is alive.
- The PAC did not connect to the emulator in pairing mode, with both the ESP32
  default BLE address and `CLONE_MAC=1`.
- The ESP32 probe scan captured 4218 advertising reports from nearby devices in
  `logs/esp32-probe-scan-realremote-20260601-010245.txt`, but no `Ganymede` and
  no `00:a0:50:*` address.
- The Android tablet BTSnoop from
  `logs/adb-bugreport-extract/BT_HCI_2026_0531_205143.cfa.curf` captured 2604
  `Ganymede` advertising reports from the real remote
  `00:A0:50:XX:XX:XX`, plus successful Android connections/pairing to that
  remote.
- The same Android bugreport records emulator/clone connections, including
  `1c:db:d4:XX:XX:XX` (test ESP32) and `00:A0:50:XX:XX:XX`.

Working hypothesis: this is not a generic ESP32 peripheral failure and not yet a
GATT-fidelity failure. The blocker is at the advertising/scanning/initiating
layer: ESP32 did not hear the real remote during probe mode, and the PAC did not
initiate to the ESP32 emulator even though Android could.

## Implementation Milestones

1. Documentation and cleanup
   - Add this plan and a log index.
   - Remove zero-byte captures, duplicate extracted snoop folders, and generated
     build output.

2. Mechanical split, no behavior change
   - Move existing code into the target modules.
   - Preserve CLI commands and JSON event names.
   - Build both roles: default emulator and `-DAPP=probe`.

3. Emulator fidelity pass
   - Add missing HID output characteristics and DIS stubs.
   - Add Environmental Sensing descriptors.
   - Add explicit tests/host-side scripts that read the expected GATT database
     from the ESP32 with nRF Connect or a second BLE central.

4. AC validation pass
   - Clear ESP32 bonds and AC remote pairing state as needed.
   - Build emulator for ESP32-S3.
   - Pair using the manual sequence.
   - After the AC subscribes to HID Report, test each `press <button>` command.
   - Capture ESP32 serial plus, if possible, Android/AC-side HCI evidence.

## Build Notes

Current standalone ESP-IDF flow:

```bash
source tools/idf-env.sh
idf.py set-target esp32s3
idf.py -DAPP=emulator build
idf.py -DAPP=probe build
idf.py -DAPP=emulator build
```

The final command is intentional after a probe build: CMake keeps `APP` in the
build cache, and the current AC validation target is the emulator.

The project uses NimBLE. Espressif's NimBLE peripheral examples explicitly cover
GATT server tables, subscribe events, advertising, and SMP configuration; the
security examples confirm bonding, MITM, Secure Connections, and connection
encryption are normal NimBLE/ESP-IDF design points:

- https://github.com/espressif/esp-idf/blob/master/examples/bluetooth/nimble/bleprph/README.md
- https://github.com/espressif/esp-idf/blob/master/examples/bluetooth/ble_get_started/nimble/NimBLE_GATT_Server/README.md
- https://github.com/espressif/esp-idf/blob/master/examples/bluetooth/ble_get_started/nimble/NimBLE_Security/README.md
