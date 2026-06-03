# Tools

- **`nrf_sniffer/`** — Phase 1: turn an nRF52840 into a Nordic **nRF Sniffer** +
  Wireshark to capture the remote↔AC pairing and resolve the AC-side SMP. See its
  `README.md`. (The ESP32 can't sniff this remote — radio limit, documented in
  `docs/re/esp32-cannot-scan-remote-investigation.md`.)

- **`linux-ble/`** — Linux/BlueZ **central** harness (merged from delonghi). Use it to
  test the nRF emulator's GATT/pairing before risking the real AC, and to drive the
  real remote as a central:
  - `set_conn_params.py` — load FAST per-device LE params (30–50 ms / superv 5 s) via
    mgmt `0x0035`; **required** — the remote only services a fast link.
  - `probe_ganymede.py` / `pair_dbus.py` — connect + Just-Works pair + GATT enumerate +
    notify capture (stop discovery before connecting; retry the `0x3e` lottery; hold the
    remote in pairing mode).
  - `scan_ganymede.py`, `scan_pairing_mode.py`, `detect_patient.py` — scan/observe.

- **`ganymede_pair_enumerate.py`** — earlier BlueZ pair+enumerate harness (pexpect over
  `bluetoothctl`); kept as a reference/alternate central test harness.

Captures land immutably under `captures/raw/`; decoded/derived output under
`captures/exports/`. Never commit unredacted keys.
