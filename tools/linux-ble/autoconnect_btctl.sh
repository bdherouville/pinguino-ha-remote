#!/usr/bin/env bash
# Single-shot background auto-connect to the sleepy Ganymede remote.
# BlueZ keeps a background (passive) connection attempt pending and completes it
# the next time the device advertises, using the slow per-device conn params we
# loaded. We poll info for ServicesResolved while the user presses the button.
set -u
MAC="${GANYMEDE_MAC:-00:A0:50:XX:XX:XX}"

# Clean any stuck pending connect.
bluetoothctl disconnect "$MAC" >/dev/null 2>&1
bluetoothctl remove "$MAC"     >/dev/null 2>&1
sleep 1

echo ">>> START PRESSING the remote button continuously and DO NOT STOP <<<"
# One discovery so BlueZ learns the device, then a single non-blocking connect
# that becomes a background auto-connect.
( bluetoothctl --timeout 8 scan on >/dev/null 2>&1 ) &
sleep 8
echo "Issuing background connect (will keep retrying in BlueZ)..."
( bluetoothctl connect "$MAC" >/tmp/btctl_connect.out 2>&1 ) &
CONN_PID=$!

resolved=0
for i in $(seq 1 60); do
  info=$(bluetoothctl info "$MAC" 2>/dev/null)
  conn=$(echo "$info" | grep -i "Connected:" | awk '{print $2}')
  srv=$(echo "$info"  | grep -i "ServicesResolved:" | awk '{print $2}')
  printf "  [%2ds] Connected=%s ServicesResolved=%s\n" "$i" "${conn:-?}" "${srv:-?}"
  if [ "${srv:-no}" = "yes" ]; then resolved=1; break; fi
  sleep 1
done

bluetoothctl scan off >/dev/null 2>&1
kill "$CONN_PID" >/dev/null 2>&1
echo "--- connect command output ---"; cat /tmp/btctl_connect.out 2>/dev/null | tail -3

if [ "$resolved" -ne 1 ]; then
  echo "RESULT: services not resolved"
  echo "=== last info ==="; bluetoothctl info "$MAC" 2>/dev/null
  exit 3
fi

echo "=== bluetoothctl info ==="; bluetoothctl info "$MAC" 2>/dev/null
echo "=== GATT attribute tree ==="; bluetoothctl gatt.list-attributes "$MAC" 2>/dev/null
