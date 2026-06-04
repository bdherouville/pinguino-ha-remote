# Tools

- **`nrf_sniffer/`** — turn an nRF52840 into a Nordic **nRF Sniffer** + Wireshark to
  capture the remote↔AC link on air. See its `README.md`. (The ESP32 can't sniff this
  remote — radio limitation, see [`../docs/HARDWARE.md`](../docs/HARDWARE.md).)

- **`linux-ble/`** — minimal Linux/BlueZ **central** harness to test the nRF emulator's
  GATT/pairing (or drive the real remote) before involving the AC. Flow:
  `set_conn_params.py` (load the fast LE params the remote needs) → `scan_ganymede.py`
  (discover) → `probe_ganymede.py` (connect + pair + enumerate + capture notifications).
  See [`linux-ble/README.md`](linux-ble/README.md).

Captures are kept **local-only** under `captures/` (git-ignored — see
`../captures/README.md`). Never commit unredacted keys.
