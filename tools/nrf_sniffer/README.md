# Phase 1 — nRF52840 BLE sniffer

**Status: working.** The sniffer passively sees the De'Longhi remote on-air (which the
ESP32 never could) — see evidence below. Use it to capture the **remote ↔ AC pairing**
(SMP) and the button reports.

## Hardware — board choice matters (32.768 kHz crystal)

The sniffer firmware is the **Nordic nRF Sniffer**, repackaged by Adafruit as a UF2 that
drag-drops onto the Adafruit UF2 bootloader (≥ 0.6.0). It is built for the nRF52840
**Dongle**, which assumes a real **32.768 kHz crystal (LFXO)**.

- ✅ **Genuine nice!nano** (USB serial `Nice_Keyboards_nice_nano…`) — has the crystal →
  works, stable. **This is the sniffer board (#1).**
- ❌ **Cheap "Pro Micro nRF52840" / "SuperMini" clones** — many omit or ship a flaky
  32 kHz crystal (see <https://github.com/joric/nrfmicro/wiki/Alternatives>). The stock
  firmware limps ~90 s then the LFCLK-dependent timing wedges and USB drops. Would need
  a firmware rebuilt for the internal RC oscillator (LFRC); not done.

Board self-IDs as "nice!nano" in `INFO_UF2.TXT` because the clones flash the nice!nano
bootloader — that string does **not** prove a crystal is present.

## Flash (no SWD, drag-drop)

1. Double-tap RESET → the **`NICENANO`** USB drive mounts.
2. Drag `firmware/sniffer_nrf52840dongle_4.1.0.uf2` onto it. It **erases the SoftDevice**,
   reboots, and enumerates as USB `1915:522a` "nRF Sniffer for Bluetooth LE"
   (`/dev/ttyACM*`). Firmware is saved under `firmware/` (from the Adafruit bootloader
   `softdevice-uf2` release).
3. To revert the board (e.g. to reuse it as the emulator): drag
   `firmware/s140_nrf52_6.1.1_softdevice.uf2` back on.

## Host setup (done, in `extcap/`)

- Nordic **nRF Sniffer for Bluetooth LE 4.1.1** extcap + `pyserial`.
- **ModemManager** will AT-probe the CDC port and can crash the sniffer. Keep it off the
  device (run once, as root):
  ```
  sudo tee /etc/udev/rules.d/99-nrf-sniffer.rules >/dev/null <<'EOF'
  ATTRS{idVendor}=="1915", ATTRS{idProduct}=="522a", ENV{ID_MM_DEVICE_IGNORE}="1", ENV{ID_MM_PORT_IGNORE}="1"
  EOF
  sudo udevadm control --reload-rules && sudo systemctl restart ModemManager
  ```
- Local fixes applied to the extcap copy: `SnifferAPI/Filelock.py` now falls back to a
  user-writable lock dir (stock hard-codes `/var/lock`, root-only here).

## Capture → `captures/raw/`

Use the helper (the stock extcap's no-Wireshark path busy-waits and writes ~0 packets —
GIL starvation; `capture.py` fixes that and auto-detects the port):
```
# ambient advertising:
python3 tools/nrf_sniffer/capture.py captures/raw/<name>.pcap <seconds>
# to capture a CONNECTION (CONNECT_IND + SMP/ATT) you MUST follow the device — plain scan
# mode does not hop with the connection:
python3 tools/nrf_sniffer/capture.py captures/raw/<name>.pcap 150 --follow 00:A0:50:XX:XX:XX
```
Open in Wireshark (DLT `NORDIC_BLE` 272) or `tshark -r <file>`. For the GUI extcap,
point Wireshark's Extcap path at `tools/nrf_sniffer/extcap/`.

## Evidence (Source for protocol Status upgrades)

`captures/raw/ganymede_sniffer_smoketest_20260603.pcap` — 6352 frames; the remote
advertises as `00:A0:50:XX:XX:XX` "Ganymede" (matches `docs/ganymede_protocol.md`):
```
tshark -r captures/raw/ganymede_sniffer_smoketest_20260603.pcap \
  -Y btle.advertising_address -T fields -e btle.advertising_address -e btcommon.eir_ad.entry.device_name | grep -i a0:50:8a
# 00:A0:50:XX:XX:XX  Ganymede
```

## Captured so far

`captures/raw/ganymede_pairing_follow_20260603.pcap` (`--follow`) — caught the **AC →
remote reconnect** on-air:
```
CONNECT_IND  00:A0:50:XX:XX:XX (AC) -> 00:A0:50:XX:XX:XX (remote)
LL_ENC_REQ / LL_ENC_RSP / LL_START_ENC_REQ   then encrypted (bad MIC, no LTK)
```
**No SMP** — the real remote is already **bonded** to the AC, so reconnect goes straight
to `LL_ENC_REQ` with the stored LTK. Confirms the AC address and the bonded-reconnect flow.

## Next: capture a FRESH pairing (the SMP)

The reconnect carries no pairing exchange. To capture the AC's **Pairing Request**
(AuthReq / IO cap / key dist) on-air, force a **fresh pair**: clear the existing bond
(remote and/or AC), then `capture.py … --follow 00:A0:50:XX:XX:XX` while the AC re-pairs.
That `.pcap` flips `docs/ganymede_protocol.md` §Pairing rows to Status `observed`. (SMP is
already known = Just Works legacy from the Android/Linux captures; this would re-confirm it.)
