# Investigation: ESP32 (ESP-IDF NimBLE) cannot scan the real Ganymede remote — 2026-06-01

## Symptom
The ESP32 BLE probe (`main/main.c`, ESP-IDF v6.0.1 NimBLE) never logs a `scan_match`
for the real De'Longhi "Ganymede" remote (`00:A0:50:XX:XX:XX`), while the same scan sees
hundreds of other advertisers.

## Decisive cross-check (Linux + attached Android + ESP32)
| Observer | Sees the **real remote**? | Sees an **nRF/continuous "Ganymede" emulator**? |
|---|---|---|
| Linux/BlueZ (laptop) | ✅ yes (−56…−74 dBm) | ✅ |
| Android (Lenovo TB-X6C6F, nRF Connect) | ✅ yes | ✅ |
| **ESP32-S3 SuperMini** (ESP-IDF NimBLE) | ❌ **never** | ✅ yes |
| **ESP32-C3-Mini** (ESP-IDF NimBLE) | ❌ **never** | ✅ yes |

**The ESP32 scan works** — it receives 800–1288 adverts/25 s from ~10–12 devices (down to
−74 dBm) **and it sees the nRF Connect emulator advertising as "Ganymede"**. The failure is
**specific to the real remote's advertising**, reproduced on **two different ESP32 chips**,
while **both** Linux and Android receive the real remote fine.

## What the real remote emits (measured via btmon)
Textbook **LE 1M legacy `ADV_IND`**: public address `00:A0:50:XX:XX:XX` (Cypress), Primary
PHY LE 1M, no secondary PHY (legacy), 25-byte AdvData = flags + name "Ganymede" + 16-bit
service UUIDs 180A/180F/181A + appearance 0x03C1. It is a **sparse, bursty, low-power**
advertiser (~3 s per button press); a normal emulator advertises continuously.

## Eliminated (each tested, all still 0)
- **RF distance/sensitivity** — remote held **touching the board**, 40 s → 0.
- **Active vs passive scan** — both; passive also can't match a scan-response name, active fixes that, neither sees the real remote.
- **Console log flood** — `LOG_ALL_ADVERTS=0` → 0.
- **Host report saturation / mbufs** — `filter_duplicates=1` (vol→13) → 0; mbuf pool 12→50 → 0.
- **Controller scan-duplicate filter** — `CONFIG_BT_CTRL_BLE_SCAN_DUPL` disabled (advert vol 800→1288) → still 0.
- **Scan interval aliasing** — `window==itvl==100 ms` and `window(60)<itvl(80)` → both 0.
- **Wrong chip / target** — esptool confirms S3 and C3; build target matches.
- **ESP32 suppressing the remote** — ESP32 logs show pure passive scan, no `connect*`/TX; Linux saw the remote *simultaneously* with the ESP32 scanning. (The "remote vanishes from nRF when ESP32 runs" effect was traced to **Linux** auto-reconnecting to its bonded remote when its adapter was powered on, and/or the active-scan/connect builds.)

## Conclusion
This is **not** a NimBLE host/config or scan-parameter problem — every host-level lever was
exhausted. The ESP32-S3/C3 **radio/controller cannot lock onto this specific real-remote
advert** (a brief, low-power Cypress `ADV_IND`), while a continuous emulator and 1000+ other
adverts come through, and Linux/Android receive the real remote. It is below the practical
reception threshold of the ESP-IDF controller for *this* advertiser on these two SuperMini-class
boards.

## Recommended next steps (hardware-level)
1. **Try the ESP32-C6 SuperMini** — project memory (`esp32-ble-probe-cli`) records the C6 as
   the board that historically *did* scan/connect/pair this remote (under PlatformIO). The C6
   radio may catch it where S3/C3 don't.
2. **Compare on-air with a real sniffer** (nRF52840 + Wireshark) capturing the real remote's
   `ADV_IND` and the ESP32's scan window simultaneously — to see exactly which adverts the
   ESP32 controller drops and why (channel, timing, RSSI at the ESP32).
3. The **emulator role is unaffected** — the A/C connects *to* the ESP32, so goal 3 does not
   depend on the ESP32 receiving the real remote.

## Current firmware state after investigation (`main/main.c`, `sdkconfig.defaults`)
Active scan, `filter_duplicates=0`, `itvl==window==100 ms`, `LOG_ALL_ADVERTS=1` (diagnostic),
controller scan-dedup disabled, MSYS_1 block count 50. (Set `LOG_ALL_ADVERTS=0` for normal use.)
