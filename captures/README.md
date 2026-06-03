# Capture archive (local-only — not committed)

The BLE captures used to reverse-engineer the Ganymede remote live **on disk only**
and are deliberately **excluded from git** (see the root `.gitignore`). Raw sniffer
captures can contain pairing/SMP material, so they are kept off the public repository.

Layout (local working tree only):

- `captures/raw/` — immutable original captures (`.pcap`, btsnoop `.log`, screenshots).
  **Never overwrite.** Subfolders: `delonghi_re/` (vendor RE btsnoops),
  `android_clone/` (Android HCI snoop of the official app).
- `captures/exports/` — decoded / filtered / redacted outputs derived from `raw/`.

The reverse-engineering *conclusions* drawn from these captures are written up — with a
`Status` and a `Source` per claim — under `docs/` (see `docs/ganymede_protocol.md` and
`docs/re/`). Those write-ups are the committed, shareable record; the raw bytes are not.

If you need to reproduce the captures, see `tools/nrf_sniffer/` (passive sniffing with
the Nordic nRF Sniffer + Wireshark).
