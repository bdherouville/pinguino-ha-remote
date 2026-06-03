#!/usr/bin/env bash
# Connect to Ganymede via bluetoothctl (BlueZ native), which handles the
# scan->connect handoff and pipelined GATT discovery better than bleak for this
# ultra-low-power sleeper. Assumes slow per-device conn params are already loaded
# (tools/linux-ble/set_conn_params.py).
#
# Keep PRESSING the remote button continuously the whole time this runs.
set -u
MAC="${GANYMEDE_MAC:-00:A0:50:XX:XX:XX}"

echo "Removing any stale cache entry for a clean attempt..."
bluetoothctl remove "$MAC" >/dev/null 2>&1

echo ">>> START PRESSING the remote button continuously now <<<"
echo "Starting background scan..."
# Background scan to make the device discoverable / catch advert windows.
coproc SCAN { bluetoothctl scan on; }
sleep 3

connected=0
for i in $(seq 1 12); do
  echo "--- connect attempt $i (keep pressing) ---"
  out=$(timeout 20 bluetoothctl connect "$MAC" 2>&1)
  echo "$out" | grep -iE "Connection successful|Failed|not available|ServicesResolved|AlreadyConnected" | head -3
  if echo "$out" | grep -qiE "Connection successful|AlreadyConnected"; then
    connected=1
    break
  fi
  sleep 1
done

# Stop scanning to free the radio for ATT once we have a link.
bluetoothctl scan off >/dev/null 2>&1
kill "${SCAN_PID:-0}" >/dev/null 2>&1

if [ "$connected" -ne 1 ]; then
  echo "RESULT: could not connect"
  exit 3
fi

echo "Connected. Waiting for service discovery to resolve (keep pressing)..."
for i in $(seq 1 20); do
  res=$(bluetoothctl info "$MAC" 2>/dev/null | grep -iE "Connected|ServicesResolved")
  echo "  [$i] $res"
  if bluetoothctl info "$MAC" 2>/dev/null | grep -qi "ServicesResolved: yes"; then
    echo "Services resolved."
    break
  fi
  if bluetoothctl info "$MAC" 2>/dev/null | grep -qi "Connected: no"; then
    echo "Dropped before services resolved."
    break
  fi
  sleep 1
done

echo "=== bluetoothctl info ==="
bluetoothctl info "$MAC" 2>/dev/null
echo "=== GATT attribute tree ==="
bluetoothctl gatt.list-attributes "$MAC" 2>/dev/null
