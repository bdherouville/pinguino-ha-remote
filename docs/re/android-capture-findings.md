# Ganymede — Android (adb logcat) capture findings — 2026-05-31

Captured live via `adb logcat` on a Lenovo TB-X6C6F (Android 12) while it scanned,
connected, and **bonded** the De'Longhi "Ganymede" remote (`00:A0:50:XX:XX:XX`).
Source log: `logs/android-cmp.txt`.

## Pairing / security (CONFIRMED)

- **Just Works pairing — `SMP_SecurityGrant() ... ENCRYPTION_ONLY`** (unauthenticated, **no MITM**, no passkey).
- **Bonding succeeds**: `bt_smp: LTK ready` → `smp_send_enc_info` → device appears in
  `dumpsys bluetooth_manager` **Bonded devices: 00:A0:50:XX:XX:XX [LE] Ganymede**.
- Bonded as a **HID host**: `HID_HOST connection policy changed: -1 -> 100`.
- ⇒ The ESP32 firmware's security config (bonding + LE Secure Connections, `sm_mitm=0`,
  `BLE_HS_IO_NO_INPUT_OUTPUT`) **matches** what the remote expects.

## GATT database (handles, from `bta_gattc_disc_res_cback`)

| Service | UUID | Handle range |
|---|---|---|
| Generic Access | 0x1800 | 0x0001–0x0007 |
| Generic Attribute | 0x1801 | 0x0008–0x000b |
| Device Information | 0x180A | 0x000c–0x001a |
| Battery | 0x180F | 0x001b–0x001d |
| Environmental Sensing | 0x181A | 0x001e–0x0036 |
| **Human Interface Device** | **0x1812** | **0x0037–0x004b** |

HID characteristics enumerated by Android's HID-over-GATT host (`bta_hh_le_search_hid_chars`):
Report Map `0x2A4B`, Report `0x2A4D` (×2), HID Information `0x2A4A`, Control Point `0x2A4C`,
Protocol Mode `0x2A4E`, Boot KB Input `0x2A22`, Boot KB Output `0x2A32`.

## Protocol (CONFIRMED shape)

The remote is a **BLE HID keyboard (HID-over-GATT, appearance 0x03C1)**. A/C commands are
sent as **HID keyboard reports** (key codes) via the HID Report characteristic `0x2A4D`
(report mode) and/or Boot Keyboard Input `0x2A22`. The exact report layout is defined by the
Report Map `0x2A4B`.

**Why Android apps (nRF) are blocked from the HID fields:** once bonded, Android's *system*
HID host (`com.android.bluetooth`, `bta_hh_le`) claims the HID service, so the HID
characteristics return `BLUETOOTH_PRIVILEGED` to ordinary apps. This is an Android policy, not
a device restriction — an ESP32 GATT client has no such layer and should be able to read
`0x2A4B`/`0x2A4D`/`0x2908` once connected+encrypted.

## Connection instability (the real obstacle)

Even on Android the link is flaky:
- Repeated `btm_sec_disconnected: Connection Failed Establishment` and `Connection Timeout`
  (handle 0x0200), and `onClientConnectionState status=133` before it finally bonded.
- After bonding, the HID host open **failed**: `BTA_HH_OPN_EVT handle=16, status=6`.
- The remote advertises only ~3 s per pairing-button (home ~7 s) press, prefers a very slow
  **4000 ms** connection interval (char `0x2A04`), and accepts **one central at a time** — so
  the originally-bonded phone / the air-conditioner steal it, and supervision timeouts drop it.

## Button-capture attempt via tablet — FAILED (decisive)

Enabled the Android HCI snoop log (Developer options) and captured a btsnoop while pressing
all 9 buttons (`logs/ganymede-btsnoop.log`, 237 s, 1600 HCI frames). Result:
**zero ACL/L2CAP/ATT frames** — only 2 stray background LE-create-connection attempts and 1
brief connection event, no data. ⇒ **The 9 functional buttons send their HID reports to the
air-conditioner (the remote's operational master), not to a bonded phone/tablet.** Bonding a
phone only exposes the GATT DB for inspection; it does not make the remote deliver button
commands there, and the link to a non-A/C central never holds long enough to carry data.

**Conclusion:** the button→report bytes only exist on the **remote↔A/C** link. Capturing them
requires a **passive BLE sniffer** (e.g. nRF52840 + Wireshark) listening to that connection
during normal use — not a GATT client on a phone or the ESP32.

## ✅ PROTOCOL CRACKED — Report Map + button reports (2026-05-31, 2nd snoop)

A second HCI-snoop capture (`logs/ganymede-btsnoop3.log`) with a connection that **held**
yielded 214 ATT packets. The HID host read the Report Map and the remote sent button reports.

### HID Report Map (`0x2A4B`, read via ATT) — raw bytes
```
05 01 09 06 a1 01 05 07 19 e0 29 e7 15 00 25 01 75 01 95 08 81 02
95 01 75 08 81 01 95 05 75 01 05 08 19 01 29 05 91 02 95 01 75 03 91 01
95 06 75 08 15 00 25 65 05 07 19 00 29 65 81 00 c0
```
Decodes to a **standard boot-keyboard report**: a single 8-byte input report, no Report ID:
`[byte0 modifier bits, byte1 reserved, byte2..byte7 = six key slots (usage 0..0x65)]`,
plus a 1-byte LED output report.

### Button reports (`0x2A4D`, notifications on value handle `0x003b`)
Each button press emits one (duplicated) notification; **no key-release report** is sent
(fire-per-press). Each button corresponds to a single bit, forming a button bitmap across the
key-array bytes: **byte2 bits 0–7 = buttons 1–8, byte3 bit0 = button 9.**

| 8-byte report value | byte/bit | captured |
|---|---|---|
| `00 00 01 00 00 00 00 00` | byte2 bit0 | ✅ |
| `00 00 02 00 00 00 00 00` | byte2 bit1 | ✅ |
| `00 00 04 00 00 00 00 00` | byte2 bit2 | ✅ |
| `00 00 08 00 00 00 00 00` | byte2 bit3 | ✅ |
| `00 00 10 00 00 00 00 00` | byte2 bit4 | ❌ missing (one press not captured) |
| `00 00 20 00 00 00 00 00` | byte2 bit5 | ✅ |
| `00 00 40 00 00 00 00 00` | byte2 bit6 | ✅ |
| `00 00 80 00 00 00 00 00` | byte2 bit7 | ✅ |
| `00 00 00 01 00 00 00 00` | byte3 bit0 | ✅ |

### ✅ COMPLETE button → report map (labeled via press-count, 3rd snoop `logs/ganymede-btsnoop4.log`)

User pressed each button an increasing number of times in a known order; run-length-encoding
the notifications gives the label for every bit:

| # | Button | Manual | Report value (8 bytes) | byte2/3 bit |
|--:|--------|:--:|------------------------|-----|
| 1 | Flap / Swing | D9 | `00 00 00 01 00 00 00 00` | byte3 bit0 |
| 2 | Silent | D2 | `00 00 80 00 00 00 00 00` | byte2 bit7 |
| 3 | Fan / air-flow | D3 | `00 00 40 00 00 00 00 00` | byte2 bit6 |
| 4 | Timer | D5 | `00 00 20 00 00 00 00 00` | byte2 bit5 |
| 5 | Eco (myEcoRealFeel) | D8 | `00 00 10 00 00 00 00 00` | byte2 bit4 |
| 6 | Mode | D6 | `00 00 08 00 00 00 00 00` | byte2 bit3 |
| 7 | UP / increase | D7 | `00 00 04 00 00 00 00 00` | byte2 bit2 |
| 8 | DOWN / decrease | D4 | `00 00 02 00 00 00 00 00` | byte2 bit1 |
| 9 | Power | D1 | `00 00 01 00 00 00 00 00` | byte2 bit0 |

So byte2 bits 0–7 = {Power, DOWN, UP, Mode, Eco, Timer, Fan, Silent}; byte3 bit0 = Flap.
Single notification per press (duplicated on air), **no key-release report** — fire-per-press.

Capture method that works: **nRF connect (hold the link) + Android HCI snoop (Developer-options
toggle) + `adb bugreport` → extract `FS/data/misc/bluetooth/logs/*.cfa.curf` (standard btsnoop)
→ `tshark -Y "btatt.opcode==0x1b && btatt.handle==0x003b" -e btatt.value`**.

This is the full v1 reverse-engineering goal achieved: GATT map, pairing method, HID report
map, and every button's report bytes. For emulation, the ESP32 (BLE HID peripheral) notifies
the corresponding 8-byte value on the Report characteristic to drive the A/C.

## Still to capture (next)

1. **Report Map `0x2A4B` bytes** — defines the exact HID report format. Needs an on-air read
   (ESP32 GATT client once connected, or an Android HCI snoop log via `adb bugreport` with
   "Enable Bluetooth HCI snoop log" turned on in Developer options).
2. **Button → key-code mapping** — capture `0x2A4D` / `0x2A22` notifications while each
   physical button is pressed (the `capture-buttons` milestone).

## Implication for the ESP32 path

Nothing about the device blocks the ESP32. To connect it must simply **win the advertising
race**: free the remote (Bluetooth OFF on every bonded phone, air-conditioner off/away),
arm the ESP32 (`connect`, which stays armed via `BLE_HS_FOREVER`), then hold the remote's home
button. Accept the slow 4000 ms interval and set a generous supervision timeout.
