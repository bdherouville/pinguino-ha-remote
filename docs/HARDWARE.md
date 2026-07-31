# Hardware & build notes

The board-specific gotchas that matter when building/flashing this project.

> **Single-board since 2026-07.** The BLE emulator now runs on the ESP32-S3's own radio
> (`firmware/esp32_bridge/main/ble_emu.c`), so a working unit is **one board**. The nRF52840
> section below still applies to the **nRF Sniffer** (needed for reverse engineering, and the
> only way to see the remote↔AC link) and to the previous two-board emulator build, which is
> kept as a known-good fallback. Protocol facts
live in [`ganymede_protocol.md`](ganymede_protocol.md); datasheets/manual in
[`references/`](references/).

## nRF52840 SuperMini (nRF Sniffer; previously also the emulator)

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
- **Why Zephyr (not Bluefruit):** ~~the AC's pairing gate checks the link-layer chip
  identity~~ — **this was wrong and is disproven.** The SoftDevice reports company `0x0059`
  (Nordic), not Cypress, and the AC pairs it anyway; the gate is the advertising
  discoverable-flags byte (see the protocol doc's *Pairing mode*). Zephyr is used because
  Bluefruit couldn't express the discoverable/SMP behaviour needed, and because the vanilla
  open-source controller hangs on this board. The clone address the emulator impersonates lives
  in the git-ignored `firmware/nrf52_emulator/zephyr/src/clone_addr.h` (template:
  `clone_addr.h.example`) — and is optional, since the address is not the gate either.
- **Bridge UART (two-board fallback only, to the ESP32):** `P0.20` RX, `P0.22` TX,
  `P0.24` heartbeat, 115200 8N1. Unused by the single-board build.
- **Status LED:** `P0.15` (driven in firmware).

## Which ESP32 can run this

Anything with **both** a BLE radio and Wi-Fi: the original ESP32, S3, C3 or C6. `board.h`
`#error`s on anything else, because the failure is otherwise confusing rather than obvious:

- **ESP32-S2 — no Bluetooth at all.** The likeliest mistake: it sits between the S1 and S3 in
  the naming and looks like a sibling, but it is Wi-Fi only.
- **ESP32-H2** — BLE and 802.15.4 but no Wi-Fi. Could be the emulator, not the bridge.
- **ESP32-P4** — no radio whatsoever; needs a companion C6 over SDIO.
- **ESP8266** — not an ESP32 and has no BLE.

`tools/identify-boards.sh` interrogates every board you plug in and tells you which it is,
including flash size (you need 4 MB+). Per-chip pins and the status LED are in
`firmware/esp32_bridge/main/board.h`; per-chip build settings in
`firmware/esp32_bridge/sdkconfig.defaults.<target>`.

## ESP32-S3 SuperMini (Wi-Fi bridge + BLE emulator)

USB-native board (`ESP32-S3FH4R2`, 4 MB flash + 2 MB PSRAM). Flash/log/debug over the
single USB-C port via USB-Serial/JTAG.

- **Status LED:** WS2812 **RGB on GPIO48** (don't reuse GPIO48 as a plain GPIO). This pin does
  not exist on the C3/C6 — hence `board.h`.
- **GPIO4 / GPIO5 / GPIO6 are now free** — they used to be the UART + heartbeat to the nRF.
- **BME280 (ambient T/H/P):** I²C — **`SDA = GPIO2`, `SCL = GPIO1`**, addr 0x76/0x77.
- **USB:** `GPIO19` (D−) / `GPIO20` (D+) are the USB lines — never repurpose them if USB is
  your only flash/log path.
- **Strapping pins to avoid driving at reset:** `GPIO0` (BOOT), `GPIO3` (JTAG src),
  `GPIO45` (VDD_SPI), `GPIO46` (boot/ROM log). On octal-PSRAM variants `GPIO33–37` may be
  reserved.
- **The ESP32 cannot *sniff* this BLE:** its radio never locks onto the remote's brief
  low-power Cypress `ADV_IND` (verified; a phone does). That is a receive-path limitation and
  it only blocks reverse engineering — use the nRF Sniffer for that. **Emulating is a different
  job:** the emulator advertises and the AC scans, so nothing has to be received. The older
  claim that "the AC never accepts an ESP32 emulator" came from a build that advertised the
  wrong discoverable flag; see the protocol doc's *Hardware reception*.
- **Wi-Fi + BLE share one radio.** Both run at once here (web UI / MQTT alongside the AC's
  ~30–50 ms BLE link), so software coexistence is enabled
  (`CONFIG_ESP_COEX_SW_COEXIST_ENABLE`). If pairing is flaky, test with Wi-Fi traffic
  saturated before blaming the BLE.
- **Flash budget:** adding NimBLE to a Wi-Fi + HTTP-server + MQTT app is a big jump. The build
  uses `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE`; if it overflows, add a custom partition
  table rather than dropping features.

## ESP32-C3 notes

Three traps, all avoided by the defaults in `board.h` — mind them if you re-pin:

- **GPIO11 is VDD_SPI and GPIO12–17 are the SPI flash.** Assigning those kills the board.
- **GPIO18/19 are the native USB D−/D+** — the port you flash and log over. The old S3 default
  put the LD2410's RX on GPIO18, which would have cost you the console.
- **Only UART0 and UART1 exist** (the S3 has three). The LD2410 therefore runs on **UART1** on
  every target now; it used UART2 only because UART1 was the nRF link.
- GPIO8 carries the LED on the DevKitM-1 and most SuperMini clones, and is also a strapping pin
  (high at reset, with GPIO9). Driving it after boot is what Espressif's own kits do.
- ~400 KB SRAM and no PSRAM, with Wi-Fi + NimBLE + HTTP server + MQTT all resident. See the
  trims in `sdkconfig.defaults.esp32c3` and watch the free-heap figure at boot.

## Wiring summary

One board. The only wiring left is the optional sensors — pin numbers are per chip (see
`board.h`); the S3 column is shown here:

```
ESP32-S3                         ESP32-C3
  GPIO1  SCL ---->  BME280 SCL     GPIO6  SCL
  GPIO2  SDA <-->   BME280 SDA     GPIO5  SDA
  GPIO17 TX  ---->  LD2410 RX      GPIO7  TX    (optional presence radar)
  GPIO18 RX  <----  LD2410 TX      GPIO10 RX
  GND    ---------  GND            GND
```

The previous two-board wiring (ESP32 `GPIO4/5/6` ↔ nRF `P0.20/P0.22/P0.24`, 115200 8N1) is
kept in git history and in the v0.3.0 release if you need to fall back to it.
