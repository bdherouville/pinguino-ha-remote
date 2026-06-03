# Ganymede BLE Pairing Notes

> **⚠️ CORRECTION (2026-06-03) — supersedes the "AC-side SMP RESOLVED" note below.**
> Hands-on emulation + a fresh Android HCI snoop (`captures/raw/android_clone/`,
> `ganymede_bt.zip`) found:
> 1. **The AC pairing a clone was NEVER captured.** `ac-btsnoop.log` and `ganymede_bt.zip`
>    are the **phone `08:38:e6…` as a CENTRAL** pairing the remote/AC — not the AC pairing
>    an emulator. The SMP **method** (Just Works, NoInputNoOutput) is right, but "Bluefruit
>    suffices for the AC to pair us" was an **unproven inference** and is **refuted**.
> 2. **Real blocker = Link-Layer identity.** The AC reads `LL_VERSION_IND` and only pairs a
>    **Cypress (`0x0131`) / BLE 4.2** device. nRF52840+SoftDevice = **Nordic (`0x0059`)/5.x**
>    → AC connects, discovers, **refuses to pair**. Not fixable from Bluefruit → **Zephyr**
>    (`CONFIG_BT_CTLR_COMPANY_ID`); pre-test a Cypress-OUI public address first.
> 3. **Device Information `0x180A`** must be implemented (real remote has full set; our port
>    advertised it but implemented nothing).
> 4. **Pairing procedure (manual "RÉPÉTER L'APPARIEMENT"):** hold remote **MODE/D6** 10 s
>    (LED D10 blinks → remote un-pairs + advertises), then hold AC **MODE/C2** 10 s until a
>    double-beep (display dot **blinks rapidly** = pairing phase), within **60 s**.
> See `ganymede_protocol.md` §Link-Layer identity / §Pairing / §Pairing procedure.

> **MERGE (2026-06-02):** the De'Longhi reverse-engineering from the former
> `~/Lab/delonghi` project is now merged. The **authoritative** protocol (advertising,
> full GATT, 61-byte Report Map, 9-button map, SMP, connection behaviour) lives in
> `ganymede_protocol.md`; the original RE analysis is preserved verbatim under
> `docs/re/` and its captures under `captures/raw/delonghi_re/`. Much of what the
> tables below mark `unknown` is now **observed** there.
>
> **AC-side SMP — RESOLVED (2026-06-02) from existing captures, no sniffer needed:**
> tshark on `captures/raw/delonghi_re/ac-btsnoop.log` shows the AC requests MITM+SC but
> **accepts Just Works legacy** vs a NoInputNoOutput responder (8 completed bonds, 0
> failures). Emulator SMP = NoInputNoOutput / SC=0 / MITM=0 / bonding / LTK+IRK+CSRK →
> Bluefruit suffices. The nRF Sniffer (Phase 1) is now just a confirmation of the real
> remote↔AC link.
>
> **Cypress mfg data — RESOLVED (2026-06-02):** tshark on `ganymede_pmode.btsnoop`
> shows the real remote's **SCAN_RSP** carries `ff 31 01 3b 04` (Company `0x0131`
> Cypress, payload `3b 04`); the **ADV_IND** does not. Emulator must put the mfg in the
> **scan response**. Also learned: **AC address = `00:A0:50:XX:XX:XX`** (Cypress).
> Only Phase-1 confirm left: whether the AC strictly *requires* the mfg to connect.
>
> The notes below are retained as the original per-capture working log.

## Device Context

| Field | Value |
|---|---|
| Remote label / model | unknown |
| Hardware revision | unknown |
| Firmware version | unknown |
| Host device | Linux BlueZ host, adapter hci0 |
| Capture date | 2026-06-01 |
| Capture timezone | Europe/Paris |
| Capture tools | `tools/ganymede_pair_enumerate.py`, `bluetoothctl`, attempted `btmon` |
| BLE adapter / sniffer | hci0, exact adapter model unknown |
| Initial bond state | observed unpaired, then paired and bonded |

## Advertisement States

### Unpaired Idle

| Field | Value |
|---|---|
| Address | 00:A0:50:XX:XX:XX |
| Address type | public |
| PDU type | unknown |
| Interval | unknown |
| RSSI | -52 to -62 dBm observed |
| Source | captures/raw/ganymede_bluetoothctl_20260601_171133.log, captures/raw/ganymede_bluetoothctl_20260601_171224.log |

Raw advertisement:

```text
AdvertisingFlags: 06
```

Raw scan response:

```text
unknown
```

Decoded AD structures:

| Type | Raw bytes | Decoded value | Notes |
|---|---|---|---|
| Flags | 06 | LE General Discoverable Mode, BR/EDR Not Supported | observed from `bluetoothctl info`; raw AD length/type bytes not available from this capture |
| Complete Local Name | unknown | Ganymede | observed as BlueZ `Name` |
| Appearance | unknown | 0x03c1 (961), icon `input-keyboard` | observed from BlueZ |
| 16-bit Service UUID | unknown | 0x180A Device Information | observed from BlueZ |
| 16-bit Service UUID | unknown | 0x180F Battery Service | observed from BlueZ |
| 16-bit Service UUID | unknown | 0x181A Environmental Sensing | observed from BlueZ |

### Pairing Mode

| Field | Value |
|---|---|
| Address | unknown |
| Address type | unknown |
| PDU type | unknown |
| Interval | unknown |
| RSSI | unknown |
| Source | unknown |

Raw advertisement:

```text
unknown
```

Raw scan response:

```text
unknown
```

Decoded AD structures:

| Type | Raw bytes | Decoded value | Notes |
|---|---|---|---|
| unknown | unknown | unknown | unknown |

### Bonded / Reconnection

| Field | Value |
|---|---|
| Address | unknown |
| Address type | unknown |
| PDU type | unknown |
| Interval | unknown |
| RSSI | unknown |
| Source | unknown |

Raw advertisement:

```text
unknown
```

Raw scan response:

```text
unknown
```

Decoded AD structures:

| Type | Raw bytes | Decoded value | Notes |
|---|---|---|---|
| unknown | unknown | unknown | unknown |

## Pairing Timeline

| Step | Timestamp / Frame | Direction | Event | Raw / Decoded Details | Source |
|---:|---|---|---|---|---|
| 1 | 2026-06-01 17:09:32 local | remote -> host | discovery | Device `00:A0:50:XX:XX:XX Ganymede` observed | captures/raw/ganymede_bluetoothctl_20260601_170932.log |
| 2 | 2026-06-01 17:12:24 local | host -> remote | pair command | `Attempting to pair with 00:A0:50:XX:XX:XX` | captures/raw/ganymede_bluetoothctl_20260601_171224.log |
| 3 | 2026-06-01 17:12:24-17:13:24 local | BlueZ state | pairing completed | `Paired: yes`, `Bonded: yes`, `Trusted: yes`, `LegacyPairing: no` | captures/exports/ganymede_enumeration_20260601_171224.json |
| 4 | 2026-06-01 17:12:24-17:13:24 local | host -> remote | connect command | Connection did not reach `Connected: yes`; GATT enumeration skipped | captures/exports/ganymede_enumeration_20260601_171224.json |

## Connection Parameters

| Field | Value | Source |
|---|---|---|
| Access address | unknown | unknown |
| Connection interval | unknown | unknown |
| Slave latency | unknown | unknown |
| Supervision timeout | unknown | unknown |
| ATT MTU | unknown | unknown |
| Data length | unknown | unknown |
| PHY | unknown | unknown |

## SMP Pairing Fields

| Field | Pairing Request | Pairing Response | Source |
|---|---|---|---|
| IO capability | unknown | unknown | HCI/SMP packets not captured |
| OOB data flag | unknown | unknown | unknown |
| Authentication requirements | unknown | unknown | unknown |
| Maximum encryption key size | unknown | unknown | unknown |
| Initiator key distribution | unknown | unknown | unknown |
| Responder key distribution | unknown | unknown | unknown |
| Pairing method | unknown | unknown | BlueZ reached paired/bonded state, but SMP method was not captured |
| LE Secure Connections | unknown | unknown | BlueZ reports `LegacyPairing: no`; exact SMP fields still need HCI capture |
| Bonding requested | unknown | unknown | final state observed as `Bonded: yes` |
| MITM requested | unknown | unknown | unknown |

## Distributed Keys

Do not commit unredacted keys.

| Key | Present | Redacted Value / Evidence | Source |
|---|---|---|---|
| LTK | unknown | not committed | BlueZ bond exists, but key store was not inspected |
| EDIV/Rand | unknown | unknown | unknown |
| IRK | unknown | unknown | unknown |
| CSRK | unknown | unknown | unknown |
| Identity address | unknown | unknown | unknown |

## GATT Database

| Handle | Type | UUID | Properties | Permissions | Initial Value | Source |
|---|---|---|---|---|---|---|
| unknown | service | 0000180a-0000-1000-8000-00805f9b34fb | unknown | unknown | unknown | BlueZ `info`, no GATT handles captured |
| unknown | service | 0000180f-0000-1000-8000-00805f9b34fb | unknown | unknown | unknown | BlueZ `info`, no GATT handles captured |
| unknown | service | 0000181a-0000-1000-8000-00805f9b34fb | unknown | unknown | unknown | BlueZ `info`, no GATT handles captured |

## Notifications, Writes, and Reads

| Timestamp / Frame | Direction | Handle / UUID | Operation | Payload | Trigger | Source |
|---|---|---|---|---|---|---|
| unknown | unknown | unknown | unknown | unknown | unknown | unknown |

## Button Event Mapping

| Control | Press Payload | Release Payload | Repeat Behavior | Timing | Source |
|---|---|---|---|---|---|
| unknown | unknown | unknown | unknown | unknown | unknown |

## Security Observations

| Claim | Status | Evidence | Notes |
|---|---|---|---|
| Pairing mode timeout | unknown | unknown | not tested |
| Accepts new pairing while bonded | unknown | unknown | unknown |
| GATT blocked before encryption | unknown | unknown | unknown |
| Button reports encrypted | unknown | unknown | unknown |
| Address privacy used | observed | public address `00:A0:50:XX:XX:XX` remained stable during these captures | needs longer idle capture |
| Replay protection present | unknown | unknown | unknown |

## Unknowns and Next Tests

| Unknown | Hypothesis | Test | Priority |
|---|---|---|---|
| GATT connection behavior | Remote may require wake/held pairing state or host-specific reconnection timing. | Keep remote awake and rerun `tools/ganymede_pair_enumerate.py --address 00:A0:50:XX:XX:XX --scan-seconds 0 --no-pair`. | high |
| Raw advertisement bytes | BlueZ info exposed decoded fields but not complete AD structures. | Rerun with working `btmon` permissions or a BLE sniffer. | high |
| SMP field decode | Pairing succeeded, but HCI/SMP packets were not captured. | Rerun pairing after deleting bond while `btmon` captures events. | high |
| Advertisement address rotation | unknown | Capture idle advertisements for at least 30 minutes. | medium |
