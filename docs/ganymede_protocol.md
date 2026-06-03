# Ganymede / De'Longhi AC — BLE Protocol Reference

Stable, evidence-traced reference for the **Ganymede** remote and its **De'Longhi
air-conditioner (AC)**. Every row carries a **Status** (`observed` / `inferred` /
`partial` / `unknown`) and a **Source**. Sources live under `captures/` and
`docs/re/` (the reverse-engineering analysis merged from the former `delonghi`
project). Raw working notes: `ganymede_pairing_notes.md`. Readiness gate:
`emulator_requirements.md`.

> **Goal:** a connected remote (nRF52840 BLE radio + ESP32 Wi-Fi) that emulates the
> manual remote — enters pairing, is visible, **accepts the AC's connection**, then
> sends the same HID button reports as the manual remote.

## Identity

| Field | Value | Status | Source |
|---|---|---|---|
| Device name | `Ganymede` | observed | docs/re/linux-capture-findings.md (btmon adv) |
| Address | `00:A0:50:XX:XX:XX`, public | observed | docs/re/linux-capture-findings.md |
| Silicon | Cypress (OUI `00:A0:50`) **CYBLE-212020-01 = PSoC 4 BLE**, Cortex-M0, **BLE 4.2** | observed | module marking + OUI; matches on-air BLE 4.2 |
| Appearance | `0x03C1` (961, HID Keyboard) | observed | docs/re/linux-capture-findings.md |
| Role | BLE **peripheral** (the AC is the central and connects to it) | observed | docs/re/android-capture-findings.md |
| AC (central) address | `00:A0:50:XX:XX:XX`, public, Cypress (OUI `00:A0:50`) | observed | tshark on captures/raw/delonghi_re/ac-btsnoop.log (LE Connection Complete peer) |

## Advertising

LE 1M **legacy `ADV_IND`** (event type `0x13` = connectable + scannable + legacy),
sparse / low-power: ~3 s burst per pairing-button press.

```text
02 01 06                          Flags: LE General Discoverable, BR/EDR Not Supported
09 09 47 61 6e 79 6d 65 64 65     Complete Local Name: "Ganymede"
07 02 0a 18 0f 18 1a 18           Incomplete 16-bit Service UUIDs: 180A, 180F, 181A
03 19 c1 03                       Appearance: 0x03C1 (Keyboard)
```

| Field | Value | Status | Source |
|---|---|---|---|
| **ADV_IND** (primary, 25 B) | Flags `06` + name `Ganymede` + UUID16 180A/180F/181A + appearance `0x03C1` — **no mfg data** | observed | docs/re/linux-capture-findings.md; tshark on captures/raw/delonghi_re/ganymede_pmode.btsnoop (event 0x0013) |
| **SCAN_RSP** | **Cypress manufacturer data: type `0xFF`, Company ID `0x0131` (Cypress), payload `3b 04`** → on air `ff 31 01 3b 04` | **observed** | tshark on ganymede_pmode.btsnoop (event 0x001b, Scan Response=True) |
| HID `0x1812` advertised? | **No** — discovered via GATT only | observed | docs/re/linux-capture-findings.md |
| AC active-scans + filters on the mfg data | **inferred** — the AC must SCAN_REQ to receive the mfg-bearing SCAN_RSP; emulator should put the Cypress mfg in its **scan response** to match. Necessity (does the AC connect without it?) = Phase-1 confirm | inferred | reasoning + emulator.c note |
| Adv interval | sparse bursts (~8 s idle tier seen via nRF, `-49 dBm`) | observed | captures/raw/delonghi_re/*, this-repo nRF screenshot |

## GATT database

| Service | UUID | Handle range | Status | Source |
|---|---|---|---|---|
| Generic Access | `0x1800` | 0x0001–0x0007 | observed | docs/re/android-capture-findings.md |
| Generic Attribute | `0x1801` | 0x0008–0x000B | observed | " |
| Device Information | `0x180A` | 0x000C–0x001A | observed | " |
| Battery | `0x180F` | 0x001B–0x001D | observed | " |
| Environmental Sensing | `0x181A` | 0x001E–0x0036 | observed | " |
| **Human Interface Device** | **`0x1812`** | **0x0037–0x004B** | observed | " |

| Characteristic | UUID | Props | Value/decode | Status | Source |
|---|---|---|---|---|---|
| HID Report Map | `0x2A4B` | R | 61-byte boot-keyboard map (below) | observed | docs/re/android-capture-findings.md (btsnoop3) |
| **HID Report (Input)** | **`0x2A4D`** | R, **Notify** | value handle **`0x003B`**, CCCD `0x2902`, Report Ref `0x2908`=`00 01` | observed | " |
| HID Information | `0x2A4A` | R | `11 01 00 02` (bcdHID 0x0111, ctry 0, flags 0x02) | observed | emulator.c / snoop |
| HID Control Point | `0x2A4C` | WNR | (unused) | observed | " |
| Protocol Mode | `0x2A4E` | R, WNR | `01` = Report mode | observed | " |
| Boot KB Input | `0x2A22` | R, Notify | all-zero idle | observed | " |
| Boot KB Output | `0x2A32` | R, W, WNR | LED out (unused) | observed | " |
| **Device Info — Manufacturer** | `0x2A29` | R | string | observed | ganymede_bt.zip (handle 0x000e) |
| **Device Info — Model Number** | `0x2A24` | R | string | observed | " (0x0010) |
| **Device Info — Hardware Rev** | `0x2A27` | R | string | observed | " (0x0012) |
| **Device Info — Serial Number** | `0x2A25` | R | string | observed | " (0x0014) |
| **Device Info — Firmware Rev** | `0x2A26` | R | string | observed | " (0x0016) |
| **Device Info — System ID** | `0x2A23` | R | 8 B | observed | " (0x0018) |
| **Device Info — PnP ID** | `0x2A50` | R | 7 B: VID source + **VID=Cypress?** + PID + version | observed (values read post-encryption, not yet decoded) | " (0x001a) |
| Battery Level | `0x2A19` | R, Notify | uint8 % | observed | " |
| Temperature | `0x2A6E` | R, Notify | sint16 LE / 100 °C | observed | " |
| Humidity | `0x2A6F` | R, Notify | uint16 LE / 100 % | observed | " |
| Pressure | `0x2A6D` | R, Notify | uint32 LE / 10 Pa | observed | " |
| Pref. Conn. Params | `0x2A04` | R | `80 0C 80 0C 00 00 B8 0B` = 4000 ms / 4000 ms / lat 0 / superv 30000 ms | observed | docs/re/android-capture-findings.md |

> **⚠️ Emulator gap (found 2026-06-03):** the current Bluefruit port **advertises**
> `0x180A` but does **not implement** the Device Information service — the AC's GATT
> discovery of the emulator shows GAP/GATT/HID/Battery/Env, **no 0x180A**. The real
> remote exposes the full Device-Info set above. **Implement it** (it may, with the
> PnP ID, be part of how the AC validates a CST remote — though the LL-version gate is
> the primary blocker).

### HID Report Map (`0x2A4B`) — 61 bytes, verbatim
```
05 01 09 06 a1 01 05 07 19 e0 29 e7 15 00 25 01 75 01 95 08 81 02
95 01 75 08 81 01 95 05 75 01 05 08 19 01 29 05 91 02 95 01 75 03 91 01
95 06 75 08 15 00 25 65 05 07 19 00 29 65 81 00 c0
```
Standard boot-keyboard: one **8-byte input report, no Report ID** + a 1-byte LED out.

## Remote inputs (button → HID report) — CONFIRMED

Each press emits **one 8-byte notification, duplicated 2× on air, no key-release**
(fire-per-press). Report = `{0, 0, byte2, byte3, 0, 0, 0, 0}`.

| Button | Manual | Report (8 bytes) | byte2/3 bit | Status |
|---|:--:|---|---|---|
| Power | D1 | `00 00 01 00 00 00 00 00` | byte2 b0 | observed |
| DOWN / decrease | D4 | `00 00 02 00 00 00 00 00` | byte2 b1 | observed |
| UP / increase | D7 | `00 00 04 00 00 00 00 00` | byte2 b2 | observed |
| Mode | D6 | `00 00 08 00 00 00 00 00` | byte2 b3 | observed |
| Eco (myEcoRealFeel) | D8 | `00 00 10 00 00 00 00 00` | byte2 b4 | observed |
| Timer | D5 | `00 00 20 00 00 00 00 00` | byte2 b5 | observed |
| Fan / airflow | D3 | `00 00 40 00 00 00 00 00` | byte2 b6 | observed |
| Silent | D2 | `00 00 80 00 00 00 00 00` | byte2 b7 | observed |
| Flap / swing | D9 | `00 00 00 01 00 00 00 00` | byte3 b0 | observed |

Source: docs/re/android-capture-findings.md (labeled via press-count in
`captures/raw/delonghi_re/ganymede-btsnoop4.log`).

## Link-Layer identity — THE PAIRING GATE (the real blocker)

**The AC checks the peripheral's chip identity BEFORE it will pair.** On-air order
(from a working pairing): `connect → LL Feature Req → LL Version Ind → SMP Pairing
Request → THEN GATT discovery`. The AC reads the remote's **LL Version Information** and
only proceeds to pair if it matches a genuine **Cypress** remote.

| Field | Real remote value | Status | Source |
|---|---|---|---|
| LMP/LL Version | **BLE 4.2 (`0x08`)** | observed | ganymede_bt.zip → captures/raw/android_clone/BT_HCI_*.curf (Read Remote Version Complete) |
| **Company / Manufacturer ID** | **`0x0131` = Cypress Semiconductor** | observed | " |
| LL Subversion | `4608` (`0x1200`) | observed | " |

**Consequence for the emulator (verified by hands-on testing 2026-06-03):** an
**nRF52840 + Nordic SoftDevice reports Company `0x0059` (Nordic) / BLE 5.x** in
LL_VERSION_IND. The AC connects to it, reads "Nordic / 5.x", and **silently refuses to
pair** — it never sends the Pairing Request and drops into discover-only. Reproduced
repeatedly; the GATT is fully discovered but no SMP occurs.

→ The LL Version Company ID is **baked into the SoftDevice and not settable from
Bluefruit/Arduino**. Faking it (Company `0x0131`, version 4.2) requires the **Zephyr /
nRF Connect SDK** controller (`CONFIG_BT_CTLR_COMPANY_ID`, version config). The address
OUI `00:A0:50` (Cypress) is a second possible check, easy to fake via a public address —
**test that first** before committing to the Zephyr port.

## Pairing / security (SMP)

| Direction | Method | Status | Source |
|---|---|---|---|
| central → **remote** (Android phone `08:38:e6…` as central) | **Just Works LEGACY**: remote responds IO=NoInputNoOutput, AuthReq Bonding (**SC=0, MITM=0**), key size 16, distributes **LTK+IRK+CSRK** | **observed** | ganymede_bt.zip + captures/raw/delonghi_re/ac-btsnoop.log (tshark `btsmp`) |
| **AC → emulated remote** | **NOT achieved.** *Correction:* the earlier "~8 AC↔clone bonds" cited as proof were mis-identified — `ac-btsnoop.log`/`ganymede_bt.zip` are the **phone (08:38:e6, central)** pairing the real remote **and** the AC, **not the AC pairing an emulator.** The AC pairing a non-Cypress peripheral has **never been observed**, and hands-on testing shows it **refuses** (see §Link-Layer identity). | **refuted** | this session's nRF emulator captures (captures/raw/ganymede_emu_*_20260603.pcap) |
| LTK between **real remote ↔ AC** | not captured (encrypted reconnect = `LL_ENC_REQ` with stored LTK; on-air shows bad-MIC without the key) | **unknown** | captures/raw/ganymede_pairing_follow_20260603.pcap |

→ SMP **method** for the responder is settled (Just Works, NoInputNoOutput, no passkey).
But that is **not** the blocker — the **LL-identity gate above is**. Bluefruit is
sufficient for the SMP method; it is **not** sufficient to pass the AC's chip-identity
check.

## Connection behaviour

| Field | Value | Status | Source |
|---|---|---|---|
| Establishment | a **lottery**: most attempts drop with HCI `0x3e` ("Connection Failed to be Established") in the ~6-event window | observed | docs/re/linux-capture-findings.md |
| Working params (central→remote) | **fast 30–50 ms interval (neg 48.75 ms), supervision 5000 ms, latency 0**, NO concurrent scan, remote held in pairing mode | observed | docs/re/linux-capture-findings.md |
| Post-bond | remote requests ~400 ms; advertises 4000 ms preferred (`0x2A04`) | observed | " |
| One central at a time | yes — a bonded phone / the AC steal the link | observed | docs/re/android-capture-findings.md |

## Pairing procedure (from the De'Longhi/Pinguino manual — "RÉPÉTER L'APPARIEMENT")

Two-sided, ordered, 60 s window:
1. **Remote first:** hold **MODE (D6)** on the CST remote ~10 s. The remote's LED **D10
   blinks** → it **un-pairs from the AC and advertises** (a "Ganymede" is only available
   to pair while unpaired+advertising — so for the emulator, *advertising = remote in
   pairing mode*; only one Ganymede should advertise at a time or the AC latches the
   other one).
2. **Then the AC:** hold **MODE (C2)** on the Pinguino unit ~10 s until a **double beep**.
   Pairing phase = **rapid blinking of the dot** in the middle of the display digits.
3. On success the AC double-beeps and the display returns to normal. **Must complete
   within 60 s.** (Test rig note: this unit's buzzer is broken → use the *rapid* dot
   blink as the pairing-mode confirmation.)

Status: observed (user + manual). Source: De'Longhi manual excerpt in
`ganymede_pairing_notes.md`.

## Hardware reception (why ESP32 fails both ways)

| Observer | Sees real remote (sniff)? | AC connects to its emulator? | Source |
|---|:--:|:--:|---|
| Linux/BlueZ | ✅ | — | docs/re/esp32-cannot-scan-remote-investigation.md |
| Android (nRF Connect) | ✅ | **unproven** — the phone paired the remote/AC *as a central*; the AC pairing an Android *peripheral* clone was never actually captured (see §Pairing correction) | docs/re/*.md |
| **ESP32-S3 / C3** | ❌ never | ❌ never | docs/re/esp32-cannot-scan-remote-investigation.md |
| **nRF52840 (Bluefruit)** | ✅ sniffs fine | ❌ AC connects + discovers but **refuses to pair** (Nordic LL identity) | this session's captures |

The ESP32-S3/C3 **radio/controller cannot lock onto** the remote's brief low-power
Cypress `ADV_IND` (host-level levers exhausted), and the AC never connects to the
ESP32 emulator — while Android (another non-ESP radio) succeeds at both. **The
ESP32-S3 SuperMini radio is the common failure factor** → BLE moves to the
**nRF52840** (sniffer + emulator); the ESP32 is Wi-Fi-only. (ESP32-C6 historically
worked under PlatformIO — noted, not pursued.)

## Reference implementation

The former delonghi NimBLE emulator (the **source of truth for the port**, not
buildable on the nRF target as-is) is preserved at
`firmware/nrf52_emulator/reference/esp-idf-nimble/main/emulator.c`: GATT table,
Report Map, 9-button table, advertising (incl. Cypress mfg data + scan-response
name), SMP config, and a full MITM/passkey handler (NUMCMP/DISP/INPUT) already
present for the AC-side-needs-MITM case.
