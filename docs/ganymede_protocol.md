# Ganymede / De'Longhi AC — BLE Protocol Reference

Stable, evidence-traced reference for the **Ganymede** remote and its **De'Longhi
air-conditioner (AC)**. Every row carries a **Status** (`observed` / `inferred` /
`partial` / `unknown`) and a **Source**. Evidence is local-only sniffer/HCI capture
(Linux/BlueZ `btmon`, Android HCI snoop, nRF Sniffer) kept under `captures/` (not
committed — see `captures/README.md`). Board/build notes: [`HARDWARE.md`](HARDWARE.md);
datasheets and the AC manual: [`references/`](references/).

> **Goal:** a connected remote (nRF52840 BLE radio + ESP32 Wi-Fi) that emulates the
> manual remote — enters pairing, is visible, **accepts the AC's connection**, then
> sends the same HID button reports as the manual remote.

## Identity

| Field | Value | Status | Source |
|---|---|---|---|
| Device name | `Ganymede` | observed | Linux/BlueZ btmon capture (btmon adv) |
| Address | `00:A0:50:XX:XX:XX`, public | observed | Linux/BlueZ btmon capture |
| Silicon | Cypress (OUI `00:A0:50`) **CYBLE-212020-01 = PSoC 4 BLE**, Cortex-M0, **BLE 4.2** | observed | module marking + OUI; matches on-air BLE 4.2 |
| Appearance | `0x03C1` (961, HID Keyboard) | observed | Linux/BlueZ btmon capture |
| Role | BLE **peripheral** (the AC is the central and connects to it) | observed | Android HCI snoop |
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
| **ADV_IND** (primary, 25 B) | Flags `06` + name `Ganymede` + UUID16 180A/180F/181A + appearance `0x03C1` — **no mfg data** | observed | Linux/BlueZ btmon capture; tshark on captures/raw/delonghi_re/ganymede_pmode.btsnoop (event 0x0013) |
| **SCAN_RSP** | **Cypress manufacturer data: type `0xFF`, Company ID `0x0131` (Cypress), payload `3b 04`** → on air `ff 31 01 3b 04` | **observed** | tshark on ganymede_pmode.btsnoop (event 0x001b, Scan Response=True) |
| HID `0x1812` advertised? | **No** — discovered via GATT only | observed | Linux/BlueZ btmon capture |
| AC active-scans + filters on the mfg data | **inferred** — the AC must SCAN_REQ to receive the mfg-bearing SCAN_RSP; emulator should put the Cypress mfg in its **scan response** to match. Necessity (does the AC connect without it?) = Phase-1 confirm | inferred | reasoning + emulator.c note |
| Adv interval | sparse bursts (~8 s idle tier seen via nRF, `-49 dBm`) | observed | captures/raw/delonghi_re/*, this-repo nRF screenshot |

## GATT database

| Service | UUID | Handle range | Status | Source |
|---|---|---|---|---|
| Generic Access | `0x1800` | 0x0001–0x0007 | observed | Android HCI snoop |
| Generic Attribute | `0x1801` | 0x0008–0x000B | observed | " |
| Device Information | `0x180A` | 0x000C–0x001A | observed | " |
| Battery | `0x180F` | 0x001B–0x001D | observed | " |
| Environmental Sensing | `0x181A` | 0x001E–0x0036 | observed | " |
| **Human Interface Device** | **`0x1812`** | **0x0037–0x004B** | observed | " |

| Characteristic | UUID | Props | Value/decode | Status | Source |
|---|---|---|---|---|---|
| HID Report Map | `0x2A4B` | R | 61-byte boot-keyboard map (below) | observed | Android HCI snoop (btsnoop3) |
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
| Pref. Conn. Params | `0x2A04` | R | `80 0C 80 0C 00 00 B8 0B` = 4000 ms / 4000 ms / lat 0 / superv 30000 ms | observed | Android HCI snoop |

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
| DOWN / decrease | D4 | `00 00 04 00 00 00 00 00` | byte2 b2 | observed (corrected) |
| UP / increase | D7 | `00 00 02 00 00 00 00 00` | byte2 b1 | observed (corrected) |
| Mode | D6 | `00 00 08 00 00 00 00 00` | byte2 b3 | observed |
| Eco (myEcoRealFeel) | D8 | `00 00 10 00 00 00 00 00` | byte2 b4 | observed |
| Timer | D5 | `00 00 20 00 00 00 00 00` | byte2 b5 | observed |
| Fan / airflow | D3 | `00 00 40 00 00 00 00 00` | byte2 b6 | observed |
| Silent | D2 | `00 00 80 00 00 00 00 00` | byte2 b7 | observed |
| Flap / swing | D9 | `00 00 00 01 00 00 00 00` | byte3 b0 | observed |

Source: Android HCI snoop (labeled via press-count in
`captures/raw/delonghi_re/ganymede-btsnoop4.log`).

> **UP/DOWN were inverted here until 2026-07.** The press-count labelling of the snoop
> swapped them; the emulator firmware that actually drives the AC uses `up`=`0x02` /
> `down`=`0x04`, and the setpoint moves the right way on the unit. The table above is the
> corrected mapping. Anything else in this file derived from press-counting deserves the same
> suspicion until re-checked against the AC's own display.

## Link-Layer identity (observed — but NOT the pairing gate)

> **Superseded:** we long believed the AC gated pairing on the peripheral's chip identity /
> Cypress address OUI. The real gate is the **discoverable flag** (see *Pairing mode* below):
> the AC pairs whatever advertises **Limited Discoverable**, regardless of address/LL identity.
> The values below are still the real remote's, but they are not what unlocks pairing.

On-air order (from a working pairing): `connect → LL Feature Req → LL Version Ind → SMP Pairing
Request → THEN GATT discovery`. The AC reads the remote's **LL Version Information**:

| Field | Real remote value | Status | Source |
|---|---|---|---|
| LMP/LL Version | **BLE 4.2 (`0x08`)** | observed | ganymede_bt.zip → captures/raw/android_clone/BT_HCI_*.curf (Read Remote Version Complete) |
| **Company / Manufacturer ID** | **`0x0131` = Cypress Semiconductor** | observed | " |
| LL Subversion | `4608` (`0x1200`) | observed | " |

Our nRF52840 + Nordic SoftDevice reports Company `0x0059` (Nordic) / BLE 5.x here, yet the AC
**pairs it fine** once the emulator advertises Limited Discoverable — so the reported LL
identity does **not** gate pairing. (Earlier "connect but never pair" results were the AC
*reconnecting a bonded address* while the emulator advertised General Discoverable, which we
misread as an LL-identity refusal.) The emulator still runs on the **Zephyr / nRF Connect
SDK** controller for other reasons (the open-source controller hangs on this board; address is
settable via `bt_ctlr_set_public_addr`). Bluefruit/Arduino is still a dead end — but for the
discoverable/SMP behaviour it couldn't express, not the LL company ID.

## Pairing / security (SMP)

| Direction | Method | Status | Source |
|---|---|---|---|
| central → **remote** (Android phone `08:38:e6…` as central) | **Just Works LEGACY**: remote responds IO=NoInputNoOutput, AuthReq Bonding (**SC=0, MITM=0**), key size 16, distributes **LTK+IRK+CSRK** | **observed** | ganymede_bt.zip + captures/raw/delonghi_re/ac-btsnoop.log (tshark `btsmp`) |
| **AC → emulated remote** | **Achieved.** When the emulator advertises **Limited Discoverable** (no bond), the AC connects and **sends the Pairing Request and bonds** (`secure=1`) — same Just Works legacy method. Address-independent; pairs/unpairs/switches like a real remote. | **observed** | nRF emulator + `captures/raw/real_remote_FULL_pairing_20260604.pcap` |

→ The responder SMP **method** is settled (Just Works, NoInputNoOutput, no passkey, key
size 16, distributes LTK+IRK+CSRK). The method was never the blocker, and neither was the
address — the **pairing-mode discoverable flag** was (see *Pairing mode* below).

## Connection behaviour

| Field | Value | Status | Source |
|---|---|---|---|
| Establishment | a **lottery**: most attempts drop with HCI `0x3e` ("Connection Failed to be Established") in the ~6-event window | observed | Linux/BlueZ btmon capture |
| Working params (central→remote) | **fast 30–50 ms interval (neg 48.75 ms), supervision 5000 ms, latency 0**, NO concurrent scan, remote held in pairing mode | observed | Linux/BlueZ btmon capture |
| Post-bond | remote requests ~400 ms; advertises 4000 ms preferred (`0x2A04`) | observed | " |
| One central at a time | yes — a bonded phone / the AC steal the link | observed | Android HCI snoop |

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

Status: observed (user + manual). Source: De'Longhi Pinguino manual
(`references/delonghi_AC_manual.pdf`).

## Hardware reception — two different questions, long conflated

| Observer | Sees real remote (sniff)? | AC pairs its emulator? |
|---|:--:|:--:|
| Linux/BlueZ | ✅ | — |
| Android | ✅ | — |
| **ESP32-S3** | ❌ never | ✅ (see below) |
| **nRF52840 (Zephyr/NCS)** | ✅ (nRF Sniffer) | ✅ bonds |

**Sniffing and emulating are separate capabilities and only the first is an ESP32 limitation.**
The ESP32-S3 radio genuinely cannot lock onto the remote's brief, sparse low-power `ADV_IND`
— for reverse engineering you need the nRF Sniffer (a controller-based scanner can't see the
remote↔AC link at all). But **emulating does not require receiving those adverts**: the
emulator is a peripheral that advertises and waits, and the AC is the one doing the scanning.

The claim that "the AC never accepts an ESP32 emulator" was an artefact of the ESP32 attempt's
own configuration, not its radio. That build
(`firmware/nrf52_emulator/reference/esp-idf-nimble/`) advertised **General** Discoverable
(`0x06`) and put the Cypress manufacturer data in the **primary** `ADV_IND`, and never sent an
SMP Security Request — i.e. it failed all three of the requirements established below and in
*Pairing mode*. It was never retested once those were understood. The bridge firmware now runs
the emulator on the ESP32-S3's own radio (`firmware/esp32_bridge/main/ble_emu.c`).

Status: the ESP32 sniffing limitation is **observed**; ESP32 emulation is **partial** — the
three known gates are implemented, on-air confirmation pending. Source: this repo's captures
plus the archived ESP32 build's advertising configuration.

## Pairing mode — the discoverable-flags gate (on-air confirmed)

A remote in **pairing mode advertises Flags `0x01` (LE Limited Discoverable)**; in normal
(bonded) operation it advertises `0x06` (General + BR/EDR-Not-Supported). The AC's pairing
scan looks for the **Limited-Discoverable** advert — *not* a specific address — then connects
and **sends the SMP Pairing Request** (the AC is the initiator). Confirmed by sniffing a full
real pairing (`captures/raw/real_remote_FULL_pairing_20260604.pcap`): Pairing Request/Response
both IO=NoInputNoOutput, **SC=0, MITM=0**, Bonding, key size 16, distributing LTK+IRK+CSRK
(Just Works legacy), no Pairing-Failed. The Cypress mfg key (`31 01 3b 04`) is in the **scan
response** (the AC SCAN_REQs it). → **The emulator must advertise Limited Discoverable (`0x01`)
when it has no bond** to be paired; General (`0x06`) when bonded for reconnect. With that, the
emulator **fresh-pairs the AC like any new remote** (no address-already-bonded requirement).

## Status — working end to end

| Step | State |
|---|:--:|
| nRF Sniffer captures the remote on-air | ✅ |
| AC discovers the emulator + walks its full GATT | ✅ |
| AC **fresh-pairs** the emulator (Limited-Discoverable advert, Just Works legacy) | ✅ |
| AC **bonds + reconnects** the emulator (Cypress-OUI public address) | ✅ |
| HID report relay (the AC never subscribes, so the report is pushed unsolicited) | ✅ |
| `press power` (and all 9 buttons) changes the AC's state, confirmed on-air | ✅ |
| LAN control: MQTT/HTTP → ESP32 → UART → nRF → AC | ✅ (two-board build) |
| Same, single board: MQTT/HTTP → ESP32-S3 → BLE → AC | ⏳ untested on the AC |

Connecting **as a central** to the real remote (for a live GATT re-read) is a lottery —
use a fast 30–50 ms interval with a ~5 s supervision timeout, **no concurrent scan**, and
hold the remote in pairing mode; most attempts drop with HCI `0x3E`, just retry
(`tools/linux-ble/`).

## Reference implementation

The former delonghi NimBLE emulator (the **source of truth for the port**, not
buildable on the nRF target as-is) is preserved at
`firmware/nrf52_emulator/reference/esp-idf-nimble/main/emulator.c`: GATT table,
Report Map, 9-button table, advertising (incl. Cypress mfg data + scan-response
name), SMP config, and a full MITM/passkey handler (NUMCMP/DISP/INPUT) already
present for the AC-side-needs-MITM case.
