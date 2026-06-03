# Ganymede — Linux (BlueZ) central capture findings — 2026-06-01

Captured from the Linux host `nuci7` (adapter `hci0`, BlueZ 5.86, kernel 7.0.10) using
`bleak` for discovery, raw `btmon` HCI snoops, and direct BlueZ mgmt/D-Bus control.
Tools live in `tools/linux-ble/`. This complements `docs/android-capture-findings.md`
(which already cracked the full protocol) by characterising the **central-side
connection behaviour** that blocks both Linux and the ESP32 probe.

## Detection (CONFIRMED, new exact bytes)

The remote is discoverable at strong RSSI (~−54 dBm) and advertises only ~3 s after a
button press. The on-air advertising payload (from `btmon`, identical in normal and
"pairing-mode" presses):

```
02 01 06                          Flags: LE General Discoverable, BR/EDR Not Supported
09 09 47 61 6e 79 6d 65 64 65     Complete Local Name: "Ganymede"
07 02 0a 18 0f 18 1a 18           Incomplete 16-bit Service UUIDs: 180A, 180F, 181A
03 19 c1 03                       Appearance: 0x03C1 (Keyboard)
```

- Advertising event type **0x0013 = Connectable + Scannable + Legacy (`ADV_IND`)**.
- Peer address **`00:A0:50:XX:XX:XX`, Public, Cypress Semiconductor** (OUI `00:A0:50`).
- **The HID service `0x1812` is NOT advertised** — only Device Info / Battery / Env
  Sensing are. The emulator must clone exactly this advert (HID is found via GATT only).

## Connection behaviour (the wall — CONFIRMED across 4 interval configs)

Every connection attempt produced the same HCI signature: the link **establishes**
(`LE Enhanced Connection Complete`, Status Success) but the peripheral then **transmits
zero packets** — no LL feature exchange, no SMP, no ATT — and the controller drops the
link with **reason `0x3e` "Connection Failed to be Established"** after the ~6-event
establishment window.

| Conn interval tried | Establishment window (6 events) | Result |
|---|---|---|
| 45 ms (BlueZ default) | ~0.27 s | 0x3e, 0 data |
| 300–600 ms | ~3.6 s | 0x3e / brief ~3 s connect, 0 data |
| 1000 ms | ~6 s | host gave up first, 0 data |
| **4000 ms** (device's published `0x2A04` preferred) | ~24 s | 0x3e, **0 data** |

Across 5 `btsnoop` captures: **0 SMP frames, 0 ATT frames, 0 ACL data frames, ever.**
Even `Device1.Pair()` (Just Works, NoInputNoOutput agent) only reached
`org.bluez.Error.AuthenticationCanceled` — BlueZ started pairing but no SMP packet ever
went on air because the peer stayed silent.

### ✅ SOLVED — Linux DID bond with the remote (2026-06-01)

Earlier drafts of this doc wrongly concluded a central "can't drive this remote." That was
my analysis error from using the **wrong connection parameters** and, critically, **running
an active discovery scan concurrently with the connect**. Corrected recipe below — Linux
bonded successfully (`Paired/Bonded/Trusted = yes`, LTK stored, encryption enabled).

**Root cause of the failures was three compounding mistakes, not the device:**

1. **Wrong connection parameters.** I used slow intervals (300 ms–4000 ms). The remote only
   services a **FAST** link. Ground truth from Android's successful bond
   (`logs/ganymede-btsnoop3.log`): **interval 30–50 ms (negotiated 48.75 ms), supervision
   timeout 5000 ms, latency 0**. (After bonding the remote itself requests ~400 ms.)
2. **Concurrent active scanning sabotaged every establishment.** Keeping BlueZ
   `StartDiscovery` (active LE scan) running while connecting starves the connection's first
   LL events → the controller drops the link at the ~6-event window with `0x3e`. With the
   scan running we held **0 of ~30** connections; Android (which does not scan while
   connecting) held **8 of 20 (~40 %)**. **STOP discovery before calling Connect/Pair.**
3. **It's still a lottery + needs pairing mode.** Even done right, most attempts get `0x3e`
   (short establishment window); just retry. The connection that bonded happened while the
   remote was **held in pairing mode** — that appears to be when it actually services an
   incoming central.

**`org.bluez.Error.AuthenticationCanceled` decoded:** it is NOT an SMP/param rejection
(no `Pairing Failed` ever goes on air). It means the *link dropped before/at SMP* — i.e.
the establishment lottery was lost on that attempt. Distinct from `AuthenticationFailed`.

### Working bonding recipe (central → remote)

```
1. Load FAST per-device conn params:  min 30ms / max 50ms / latency 0 / supervision 5000ms
   (tools/linux-ble/set_conn_params.py — mgmt Load Connection Parameters 0x0035)
2. Register a NoInputNoOutput agent (Just Works).
3. Discover the device ONCE, then STOP discovery (no active scan during connect).
4. Hold the remote in PAIRING MODE; call Device1.Pair() repeatedly until one link holds.
5. Most attempts drop with 0x3e — retry. On success: SMP runs instantly, bonds, LTK stored.
```
Tool: `tools/linux-ble/detect_patient.py` (event-driven, stops scan before connect).

### Confirmed SMP / bonding parameters (the remote as peripheral)

From Android's `ganymede-btsnoop3.log` (and reproduced by our successful bond):
- Remote Pairing Response: **IO = NoInputNoOutput, AuthReq = 0x01 (Bonding; SC=0, MITM=0)**
  → negotiated **LEGACY (not LE Secure Connections) Just Works, unauthenticated**.
- Max encryption key size **16**. Remote distributes **LTK + IRK + CSRK**.
- A central should offer Just Works (NoInputNoOutput), bonding, and accept the legacy
  downgrade. BlueZ must NOT be in "Secure Connections Only" mode (ours was not).

> NOTE — this is the OPPOSITE of the A/C-facing side: when the **A/C** (central) pairs to
> the **emulated** remote it demands **MITM=1 + LE Secure Connections** (see memory
> `ac-smp-requirements`). The ESP32 thus needs different SMP config per role:
> probe/central→remote = Just Works legacy (MITM 0, SC 0); emulator/peripheral←A/C = MITM 1, SC 1.

### Consequence for the ESP32

- **The central/probe approach is viable after all** — the ESP32 probe must (a) request a
  **fast 30–50 ms interval with a 5 s supervision timeout**, (b) **not run a separate scan
  while connecting**, (c) retry through the `0x3e` lottery, ideally with the remote in
  pairing mode. This likely explains the old probe failures.
- The emulator (peripheral) milestone is unaffected and still uses the known advert + HID
  report map + 9 button reports.
- A live GATT re-read from Linux is achievable the same way but is only validation — the
  HID report map and button bytes are already in `android-capture-findings.md`.

## What DID work / was learned for the central path

1. **Slow per-device connection params are required and effective.** BlueZ's default
   45 ms / 420 ms link can't even hold for the establishment window. Loading per-device
   params via mgmt **`Load Connection Parameters` (0x0035)** — scoped to the one address,
   runtime only — makes the link form cleanly (negotiated 600 ms / 6 s, 4000 ms / 32 s).
   See `tools/linux-ble/set_conn_params.py`. The ESP32 central must likewise request a
   slow interval + long (≥6 s, ideally matching the 30 s `0x2A04`) supervision timeout.
2. **Discovery vs. connection must not overlap.** Active scanning during the (sub-Hz)
   connection starves ATT and makes BlueZ abort service discovery. Use scan→stop→connect,
   or BlueZ background auto-connect (passive scan).
3. **bleak sees the device reliably; `bluetoothctl` got stuck** in an orphaned
   "Discovering: yes" state (background auto-connect ref). Prefer bleak or direct D-Bus.

## The decisive remaining test (to get a live Linux/ESP32 GATT read)

The protocol is already fully known (report map + all 9 button bytes are in
`android-capture-findings.md`). To prove a generic central can *hold* the link and read
GATT, the remote must first be **freed from its competing master**:

1. **Air-conditioner OFF / unplugged / out of range.**
2. **Bluetooth OFF on every previously-bonded phone/tablet.**
3. Load the slow params (`sudo .venv-ble/bin/python tools/linux-ble/set_conn_params.py`),
   start `btmon`, then run `pair_dbus.py` (or `probe_ganymede.py`) and hold the remote's
   button. With no competitor, the same connection that produced 0x3e should now carry
   SMP → encryption → ATT and resolve the HID service.

## Tooling produced (`tools/linux-ble/`)

| File | Purpose |
|---|---|
| `scan_ganymede.py` | Continuous scan, dump advert payload |
| `scan_pairing_mode.py` | Observe advert variants while held in pairing mode |
| `set_conn_params.py` | Load slow per-device LE conn params via mgmt (0x0035) |
| `probe_ganymede.py` | bleak connect + pair + full GATT enumerate + notify capture |
| `pair_dbus.py` | Pure BlueZ D-Bus: Just-Works agent + `Pair()` + GATT dump |
| `autoconnect_btctl.sh`, `pair_btctl*.sh`, `connect_btctl.sh` | bluetoothctl drivers |
| `ganymede_hci*.btsnoop` | raw HCI captures (`btmon -r` to decode) |

Captures use a venv at `.venv-ble/` (bleak 3.0.2, BlueZ D-Bus backend).
