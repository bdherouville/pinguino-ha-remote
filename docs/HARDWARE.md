# Hardware & build notes

The board-specific gotchas that matter when building/flashing this project. Protocol facts
live in [`ganymede_protocol.md`](ganymede_protocol.md); datasheets/manual in
[`references/`](references/).

## nRF52840 SuperMini (BLE emulator + sniffer)

A nice!nano-v2-compatible "Pro Micro" clone. Cheap and flaky — buy spares.

- **Faulty 32.768 kHz crystal** (common on these clones): the external LFXO never
  stabilises, so the BLE controller won't start. Use the RC oscillator —
  `CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y` (already set in the out-of-tree board defconfig).
  The 32 MHz HFXO is fine.
- **Bootloader / flashing:** Adafruit UF2 bootloader. No reset button — enter the
  bootloader with an **RST↔GND double-short**, or from the running emulator the shell
  command **`emu dfu`** (soft-reboot to the UF2 drive). Flash by copying
  `build_ncs/zephyr/zephyr.uf2` onto the `NICENANO` drive. **Do not short the power pads**
  (kills the board).
- **App flash offset `0x1000`** (no SoftDevice layout). Images linked at `0x26000` jump to
  empty flash and nothing runs — the board's defconfig/partitioning pins it at `0x1000`.
- **Bootloader upgrade (one-time, required on a new board).** Boards ship with an old/SoftDevice
  bootloader that can't boot the `0x1000` no-SoftDevice app. Enter the bootloader and drag
  **`update-nice_nano_bootloader-0.9.2_nosd.uf2`** (the *no-SoftDevice* variant from
  [Adafruit_nRF52_Bootloader 0.9.2](https://github.com/adafruit/Adafruit_nRF52_Bootloader/releases/tag/0.9.2),
  also attached to our releases) onto the `NICENANO` drive, then flash the app. We upgraded
  0.6.0 → 0.9.2 this way; older 2021 bootloaders were too old to take the app.
- **Toolchain:** nRF Connect SDK (Zephyr) with Nordic's SoftDevice Controller (`sdc`/MPSL).
  The vanilla open-source controller (`BT_LL_SW_SPLIT`) hangs at init on this board.
- **Why Zephyr (not Bluefruit):** the AC's pairing gate checks the link-layer chip
  identity; the SoftDevice's company ID isn't settable from Bluefruit. See the protocol
  doc's "Link-Layer identity" section. The clone address the emulator impersonates lives in
  the git-ignored `firmware/nrf52_emulator/zephyr/src/clone_addr.h` (template:
  `clone_addr.h.example`).
- **Bridge UART (to the ESP32):** `P0.20` RX, `P0.22` TX, `P0.24` heartbeat, 115200 8N1.
- **Status LED:** `P0.15` (driven in firmware).

## ESP32-S3 SuperMini (Wi-Fi bridge)

USB-native board (`ESP32-S3FH4R2`, 4 MB flash + 2 MB PSRAM). Flash/log/debug over the
single USB-C port via USB-Serial/JTAG.

- **Status LED:** WS2812 **RGB on GPIO48** (don't reuse GPIO48 as a plain GPIO).
- **Bridge UART (to the nRF):** `GPIO4` TX → nRF RX, `GPIO5` RX ← nRF TX, `GPIO6` heartbeat.
  (Chosen over the default UART0 `GPIO43/44` and the USB lines.)
- **BME280 (ambient T/H/P):** I²C — **`SDA = GPIO2`, `SCL = GPIO1`**, addr 0x76/0x77.
- **USB:** `GPIO19` (D−) / `GPIO20` (D+) are the USB lines — never repurpose them if USB is
  your only flash/log path.
- **Strapping pins to avoid driving at reset:** `GPIO0` (BOOT), `GPIO3` (JTAG src),
  `GPIO45` (VDD_SPI), `GPIO46` (boot/ROM log). On octal-PSRAM variants `GPIO33–37` may be
  reserved.
- **The ESP32 cannot do this BLE:** its radio never locks onto the remote's brief low-power
  Cypress `ADV_IND`, and the AC never accepts an ESP32 emulator (both verified; a phone — a
  different radio — does both). So the ESP32 is **Wi-Fi-only** and the nRF owns the BLE.

## Wiring summary

```
ESP32-S3            nRF52840
  GPIO4  TX  ---->  P0.20 RX
  GPIO5  RX  <----  P0.22 TX
  GPIO6  HB  <----  P0.24 (heartbeat ~1 Hz)
  GPIO1  SCL ---->  BME280 SCL
  GPIO2  SDA <-->   BME280 SDA
```
