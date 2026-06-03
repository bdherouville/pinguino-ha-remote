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
| `scan_ganymede.py` / `scan_pairing_mode.py` | discover the remote / catch it in pairing mode |
| `probe_ganymede.py` | connect + enumerate services / characteristics / descriptors |
| `pair_dbus.py`, `pair_btctl.sh`, `pair_btctl2.sh` | pair via D-Bus / `bluetoothctl` |
| `connect_btctl.sh`, `autoconnect_btctl.sh` | connect / auto-reconnect |
| `set_conn_params.py` | adjust connection parameters |
| `detect_patient.py`, `sync_signal_test.py` | connection/timing experiments |

Requires BlueZ + `python3-dbus`. Keep the host adapter's other bonds out of the way so it
doesn't auto-steal the remote (`bluetoothctl remove <addr>` / `rfkill`).
