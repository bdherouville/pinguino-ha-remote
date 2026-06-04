# Linux / BlueZ central harness

Small scripts that drive a Linux host (via `bluetoothctl` and BlueZ D-Bus) as a BLE
**central** — used to scan, connect, pair, and probe the Ganymede remote (and to exercise
the nRF emulator's GATT/pairing before risking the real AC).

## Target address

The scripts target a single device address. It is **not hardcoded** — set it once via the
`GANYMEDE_MAC` environment variable (default placeholder `00:A0:50:XX:XX:XX`):

```bash
export GANYMEDE_MAC=00:A0:50:AA:BB:CC   # your remote's / emulator's address
./scan_ganymede.py
```

Find the address by scanning: `bluetoothctl scan le` (or `./scan_ganymede.py`) and look for
the `Ganymede` device — De'Longhi remotes use the Cypress OUI `00:A0:50`.

## Scripts

| Script | Purpose |
|--------|---------|
| `set_conn_params.py` | load the fast per-device LE conn params (30–50 ms / 5 s supervision) the remote needs — **run first** |
| `scan_ganymede.py` | discover the remote / emulator and dump its advertising payload |
| `probe_ganymede.py` | connect + pair + enumerate the full GATT + capture HID notifications |

Typical flow: `set_conn_params.py` → `scan_ganymede.py` → `probe_ganymede.py`.

Requires BlueZ + `bleak`. Connecting to the real remote is a lottery (most attempts drop
with HCI `0x3E`) — retry, with no concurrent scan and the remote in pairing mode. Keep the
host adapter's other bonds out of the way so it doesn't auto-steal the remote
(`bluetoothctl remove <addr>` / `rfkill`).
