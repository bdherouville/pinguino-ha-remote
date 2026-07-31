#!/usr/bin/env bash
# Identify every ESP32 board currently plugged in, and say whether each one can run this
# project's firmware (Wi-Fi bridge + BLE remote emulator on a single chip).
#
# Usage:  tools/identify-boards.sh
#         tools/identify-boards.sh /dev/cu.usbmodem1101     # just one port
#
# Needs esptool:  pip install esptool
#
# Boards are reset into the ROM bootloader over DTR/RTS to be interrogated, so whatever they
# were running stops. Unplug/replug (or reset) to get the firmware going again. Nothing is
# written to any board — this only reads.

set -uo pipefail

# ---- what each chip can do -------------------------------------------------------------
# This project needs, on ONE chip: BLE (peripheral) + Wi-Fi + ~1.8 MB of app flash.
verdict() {
    case "$1" in
    esp32)    echo "OK|BT Classic + BLE 4.2, Wi-Fi. Older and power-hungry, but it works." ;;
    esp32s3)  echo "OK|BLE 5.0 + Wi-Fi. The reference target for this project." ;;
    esp32c3)  echo "OK|BLE 5.0 + Wi-Fi. Fine; less RAM than an S3." ;;
    esp32c6)  echo "OK|BLE 5.0 + Wi-Fi 6. Best of the bunch if flash is 8 MB+." ;;
    esp32c5)  echo "OK|BLE 5 + dual-band Wi-Fi. Should work; untested here." ;;
    esp32c2|esp8684)
              echo "MAYBE|Has BLE + Wi-Fi, but these modules usually ship 1-2 MB flash — too small." ;;
    esp32s2)  echo "NO|ESP32-S2 has NO Bluetooth at all. Wi-Fi only. Cannot be the emulator." ;;
    esp32h2)  echo "NO|BLE + 802.15.4 but NO Wi-Fi — it could be the emulator, not the bridge." ;;
    esp32p4)  echo "NO|No radio at all. Needs a companion C6 over SDIO (esp-hosted)." ;;
    esp8266)  echo "NO|Not an ESP32 and has no BLE whatsoever." ;;
    *)        echo "?|Unrecognised chip id '$1' — check the ESP-IDF docs for its radio." ;;
    esac
}

command -v esptool >/dev/null 2>&1 && ESPTOOL=esptool || ESPTOOL=esptool.py
if ! command -v "$ESPTOOL" >/dev/null 2>&1; then
    echo "esptool not found. Install it with:  pip install esptool" >&2
    exit 1
fi

# ---- find candidate ports --------------------------------------------------------------
if [ $# -gt 0 ]; then
    ports=("$@")
else
    # macOS: /dev/cu.* (never /dev/tty.* — those block on open waiting for carrier detect).
    # Linux: ttyUSB* (CP2102/CH340 bridges) and ttyACM* (native-USB chips).
    mapfile -t ports < <(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial* \
                            /dev/ttyUSB* /dev/ttyACM* 2>/dev/null)
fi

if [ ${#ports[@]} -eq 0 ]; then
    echo "No serial ports found. Plug a board in."
    echo "Boards with a native-USB chip (S2/S3/C3/C6) appear on their own; the original ESP32"
    echo "needs a USB-UART bridge, so check the cable is a data cable and the driver is present."
    exit 0
fi

printf '%-24s %-12s %-8s %-6s %s\n' PORT CHIP FLASH USABLE NOTES
printf '%-24s %-12s %-8s %-6s %s\n' ------------------------ ------------ -------- ------ -----
for port in "${ports[@]}"; do
    # flash_id reports the chip type AND the flash size in one go. Try both spellings:
    # esptool v5 renamed subcommands to dashes.
    out=$("$ESPTOOL" --port "$port" flash_id 2>&1) \
        || out=$("$ESPTOOL" --port "$port" flash-id 2>&1) \
        || true

    # "Detecting chip type... ESP32-S3" / "Chip is ESP32-S3 (QFN56) (revision v0.2)"
    chip=$(printf '%s\n' "$out" | sed -n 's/^Chip is \([A-Za-z0-9-]*\).*/\1/p' | head -1)
    [ -z "$chip" ] && chip=$(printf '%s\n' "$out" | sed -n 's/^Detecting chip type\.*[[:space:]]*//p' | head -1)
    flash=$(printf '%s\n' "$out" | sed -n 's/^Detected flash size: //p' | head -1)

    if [ -z "$chip" ]; then
        printf '%-24s %-12s %-8s %-6s %s\n' "$port" "-" "-" "-" \
               "not an ESP32 in bootloader mode (or port busy)"
        continue
    fi

    # esp32-s3 -> esp32s3, for the case statement
    key=$(printf '%s' "$chip" | tr 'A-Z' 'a-z' | tr -d '-')
    IFS='|' read -r ok note <<<"$(verdict "$key")"

    # Flash too small for Wi-Fi + BLE + HTTP + MQTT, whatever the chip.
    case "${flash:-}" in
        1MB|2MB) ok="NO"; note="Only ${flash} flash — this firmware needs 4 MB+." ;;
    esac

    printf '%-24s %-12s %-8s %-6s %s\n' "$port" "$chip" "${flash:-?}" "$ok" "$note"
done

cat <<'EOF'

USABLE = can run the whole project (Wi-Fi bridge + BLE emulator) on that one chip.
A "NO" on Bluetooth or Wi-Fi is a hard silicon limit, not a firmware setting.

No serial port available? Read the metal can on the module instead:
  ESP32-WROOM-32 / ESP32-WROVER   -> original ESP32   (BLE, works)
  ESP32-S2-...                    -> S2               (NO Bluetooth)
  ESP32-S3-WROOM-1 / -FH4R2       -> S3               (works)
  ESP32-C3-MINI-1 / -FH4          -> C3               (works)
  ESP32-C6-WROOM-1                -> C6               (works)
  ESP32-H2-...                    -> H2               (no Wi-Fi)
  ESP8266 / ESP-12E / ESP-01      -> not an ESP32     (no BLE)
EOF
