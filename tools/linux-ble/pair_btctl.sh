#!/usr/bin/env bash
# Pair/bond with Ganymede while it is held in its PAIRING MODE, then dump the
# full (cached) GATT. In pairing mode the remote should accept SMP from a new
# central; once bonded, BlueZ caches services and we can read HID attributes.
# Slow per-device conn params must already be loaded (set_conn_params.py).
set -u
MAC="${GANYMEDE_MAC:-00:A0:50:XX:XX:XX}"

bluetoothctl disconnect "$MAC" >/dev/null 2>&1
bluetoothctl remove "$MAC"     >/dev/null 2>&1
sleep 1

# Just Works bonding -> NoInputNoOutput agent auto-accepts.
bluetoothctl <<'EOF' >/dev/null 2>&1
agent NoInputNoOutput
default-agent
EOF

echo ">>> PUT THE REMOTE IN PAIRING MODE NOW and hold it there <<<"
echo "Scanning + pairing (this drives connect+SMP+bond)..."
( bluetoothctl --timeout 10 scan on >/dev/null 2>&1 ) &
sleep 6

ok=0
for i in $(seq 1 8); do
  echo "--- pair attempt $i (stay in pairing mode) ---"
  out=$(timeout 25 bluetoothctl pair "$MAC" 2>&1)
  echo "$out" | grep -iE "Pairing successful|Failed|AuthenticationFailed|not available|AlreadyExists|Connected" | head -4
  if echo "$out" | grep -qiE "Pairing successful|AlreadyExists"; then ok=1; break; fi
  sleep 1
done
bluetoothctl scan off >/dev/null 2>&1

if [ "$ok" -ne 1 ]; then
  echo "RESULT: pairing did not complete"
  echo "=== info ==="; bluetoothctl info "$MAC" 2>/dev/null
  exit 3
fi

echo "Paired. Marking trusted + waiting for service discovery..."
bluetoothctl trust "$MAC" >/dev/null 2>&1
for i in $(seq 1 25); do
  if bluetoothctl info "$MAC" 2>/dev/null | grep -qi "ServicesResolved: yes"; then break; fi
  sleep 1
done

echo "=== bluetoothctl info ==="; bluetoothctl info "$MAC" 2>/dev/null
echo "=== GATT attribute tree ==="; bluetoothctl gatt.list-attributes "$MAC" 2>/dev/null
