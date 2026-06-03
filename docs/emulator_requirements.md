# Emulator Requirements — readiness gate

The connected remote (nRF52840 BLE peripheral) must not be considered done until
each required row is **known** or **intentionally unsupported**. Status/Source per
`ganymede_protocol.md`.

> **HARD GATE (found 2026-06-03): the AC validates the peripheral's Link-Layer chip
> identity before pairing.** It reads `LL_VERSION_IND` and only pairs a **Cypress
> (`0x0131`), BLE 4.2** device. An nRF52840+SoftDevice reports **Nordic (`0x0059`)/5.x**
> → the AC connects, discovers, and **refuses to pair**. This — not the SMP method —
> is the blocker, and it is **not fixable from Bluefruit** (SoftDevice hard-codes the
> company ID). See the new "Link-Layer identity" row below.

## BLE role / radio

| Requirement | Value | Status |
|---|---|---|
| Role | BLE **peripheral** on **nRF52840** (Bluefruit); the AC connects to it | observed |
| Why not ESP32 | ESP32-S3/C3 radio can't be seen by the AC nor sniff the remote; Android (non-ESP radio) works → use nRF52840 | observed |
| Address policy | nRF **own address**, **fresh pairing** (AC in pairing mode) — Android clone worked this way; MAC-clone risks a stale AC bond | observed |

## Advertising

| Requirement | Value | Status |
|---|---|---|
| ADV_IND payload | 25 B: Flags `06` + name `Ganymede` + UUID16 180A/180F/181A + appearance `0x03C1` (no mfg) | observed |
| **Scan response** | **Cypress mfg `ff 31 01 3b 04`** (company `0x0131`, payload `3b 04`) — put it HERE, not in the primary ADV, to match the real remote | observed |
| AC scans + filters on mfg | AC active-scans (SCAN_REQ) to read the scan response; include the mfg to be safe | inferred |
| Adv params | fast connectable-undirected (20–40 ms) so the AC's short pairing scan catches it | observed |

## Link-Layer identity — THE GATE

| Requirement | Value | Status |
|---|---|---|
| **LL_VERSION_IND Company ID** | **`0x0131` Cypress** (nRF/SoftDevice = `0x0059` Nordic → AC refuses) | **observed = hard requirement** |
| LL/LMP version | **BLE 4.2 (`0x08`)** (nRF reports 5.x) | observed |
| **Implication** | needs **Zephyr/nRF Connect SDK** (`CONFIG_BT_CTLR_COMPANY_ID=0x0131`, version 4.2); Bluefruit/SoftDevice **cannot** | decided |
| Cheaper pre-test | set a **Cypress-OUI public address `00:A0:50:xx:xx:xx`** (not the real one) — if the AC then pairs, the gate was the address OUI, not the LL version | **to test** |

## Pairing / security

| Requirement | Value | Status |
|---|---|---|
| AC-side SMP method | **Just Works legacy**, NoInputNoOutput, no passkey. *Note:* the "8 bonds in `ac-btsnoop.log`" were the **phone(central)↔remote/AC**, NOT the AC pairing a clone — the SMP **method** is right but never proved the AC pairs an emulator | observed (method) / **refuted** (AC↔emulator) |
| IO capability | **NoInputNoOutput** (Bluefruit handles the SMP method fine) | observed |
| Keys / bond | responder distributes LTK+IRK+CSRK (key dist 0x07), key size 16 | observed |
| Encrypted GATT | HID reads/notifies after encryption | observed (general HoG) |

## GATT database

| Requirement | Value | Status |
|---|---|---|
| Services | 1800/1801/180A/180F/181A/**1812** | observed |
| **Device Info `180A`** | **MUST implement** Mfr `2A29`, Model `2A24`, HW `2A27`, Serial `2A25`, FW `2A26`, SystemID `2A23`, **PnP ID `2A50`** — current Bluefruit port advertises 180A but **implements none of it** | **observed gap — TODO** |
| HID chars | Report Map `2A4B`, Report `2A4D` (val 0x003B, CCCD, Report Ref `00 01`), Info `2A4A`, Ctrl `2A4C`, Proto Mode `2A4E`, Boot KB In/Out `2A22`/`2A32` | observed |
| Report Map | 61-byte boot-keyboard map | observed |
| Battery / Env | `2A19`; `2A6E`/`2A6F`/`2A6D` (sint16/uint16/uint32) | observed |

## Input emulation

| Input | Payload | Status |
|---|---|---|
| 9 buttons (power/down/up/mode/eco/timer/fan/silent/flap) | 8-byte report `{0,0,b2,b3,0,...}`, 2× per press, no release | observed |

## Validation tests (the `/goal`)

| Test | Expected | Status |
|---|---|---|
| Sniffer works | nRF Sniffer captures the remote on-air | ✅ **done** (captures/raw/ganymede_sniffer_smoketest_20260603.pcap) |
| AC discovers emulator | AC connects to the nRF advertising as Ganymede + walks full GATT | ✅ **done** (the AC fully discovers our GATT) |
| AC bonds with emulator | encryption + bond | ❌ **BLOCKED** — AC refuses to pair (LL identity = Nordic, not Cypress) |
| Cypress-OUI address pre-test | AC pairs after public addr `00:A0:50:xx` | **next** |
| Zephyr port w/ Cypress LL identity | AC sends Pairing Request + bonds | **next, if pre-test fails** |
| AC subscribes to HID Report | CCCD `01 00` on Report char | pending (after bond) |
| `press power` acts on the AC | AC changes state (confirmed on-air by the sniffer) | pending |
| All 9 buttons accepted | each maps to the correct AC action | pending |
| LAN control (Phase 3) | MQTT/HTTP command → UART → nRF → AC acts | ✅ chain wired & verified (ESP↔UART↔nRF↔BLE); gated on the bond above |
