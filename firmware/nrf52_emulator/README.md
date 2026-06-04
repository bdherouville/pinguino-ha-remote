# nRF52840 emulator — the BLE radio

A BLE **peripheral** that impersonates the De'Longhi "Ganymede" remote so the **AC connects
to it** and receives the same HID button reports. This is where the BLE lives (the ESP32
can't — see [`../../docs/HARDWARE.md`](../../docs/HARDWARE.md)).

The working firmware is the **Zephyr / nRF Connect SDK** app under [`zephyr/`](zephyr/). The
Arduino/Bluefruit sketch in `src/` and `reference/esp-idf-nimble/` are kept only as
historical reference — they **cannot** pass the AC's link-layer identity gate (see
[`../../docs/ganymede_protocol.md`](../../docs/ganymede_protocol.md)).

---

## Flash it — the fast path (prebuilt UF2)

Grab `ganymede-emulator-nrf52840.uf2` and `update-nice_nano_bootloader-0.9.2_nosd.uf2` from the
[latest release](https://github.com/bdherouville/pinguino-ha-remote/releases/latest).

> These SuperMini boards have **no reset button**. Enter the bootloader by briefly bridging
> the **RST and GND pads twice in quick succession** (a "double-tap reset"). A USB drive named
> **`NICENANO`** appears.

1. **Upgrade the bootloader (one-time, required).** Boards ship with an old bootloader that
   can't boot our no-SoftDevice app. Enter the bootloader, then drag
   **`update-nice_nano_bootloader-0.9.2_nosd.uf2`** onto the `NICENANO` drive. It re-flashes and
   re-enumerates. *(Skip only if you know the board already has a recent no-SoftDevice
   bootloader.)*
2. **Flash the app.** Enter the bootloader again (double-tap reset), then drag
   **`ganymede-emulator-nrf52840.uf2`** onto `NICENANO`. The board reboots into the emulator.

That's it — the board now advertises as `Ganymede`. The on-board LED blinks while advertising
and goes solid once the AC bonds. (After the app is running, the USB shell command `emu dfu`
re-enters the bootloader without the pad trick.)

The prebuilt binary **generates a unique per-device Cypress-OUI address** at first start (from
the chip's factory device ID, stable across reboots) — so every unit is its own remote. The
**address is not the pairing gate**: the AC pairs whatever advertises in **pairing mode** (the
emulator advertises **Limited Discoverable** when it has no bond, exactly like a real remote).
So the binary **pairs like any new remote** — put the AC in pairing mode and it bonds (or use
the **Pair / Unpair** buttons in the bridge web UI). See *Pair with the AC* in the
[main README](../../README.md).

---

## Build it locally (nRF Connect SDK v2.7.0)

```bash
# in your NCS workspace (west + the Zephyr SDK installed)
APP=/path/to/pinguino-ha-remote/firmware/nrf52_emulator/zephyr
west build -b nice_nano/nrf52840 "$APP" -d "$APP/build_ncs" -- -DBOARD_ROOT="$APP"
# flash: copy build_ncs/zephyr/zephyr.uf2 to the NICENANO drive (see bootloader entry above)
```

The app links at flash offset **0x1000** (no SoftDevice) and uses the **RC** low-frequency
clock (these clones have a flaky 32 kHz crystal). Both are pinned in the out-of-tree board
under `zephyr/boards/`. Details in [`../../docs/HARDWARE.md`](../../docs/HARDWARE.md).

### Cloning a *specific* remote's address (optional, advanced)

You almost never need this — the generic address pairs fine. The only reason to clone a
specific address is to **silently take over an AC's *existing* bond** (so the AC encrypted-
reconnects to your emulator without a fresh pairing), by impersonating the exact address that
AC already trusts. The address is kept in a git-ignored header so it never ships in the repo:

```bash
cp zephyr/src/clone_addr.h.example zephyr/src/clone_addr.h
# edit CLONE_ADDR_LE — little-endian; keep 0x50,0xa0,0x00 as the last three bytes (Cypress OUI).
# find the address with tools/linux-ble/scan_ganymede.py
```

Without `clone_addr.h` the build falls back to the `.example` template (the generic address).

## USB shell

`emu status` (conn/secure/subscribed), `emu press <btn>`, `emu adv`, `emu clear_bonds`,
`emu dfu`, `emu gatt`. Buttons: `power down up mode eco timer fan silent flap`.
