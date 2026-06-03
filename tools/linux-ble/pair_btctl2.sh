#!/usr/bin/env bash
# Keep an active scan running the WHOLE time so the device object never goes
# "not available", and repeatedly attempt pair while the remote is held in
# pairing mode. Watch for SMP progression in the parallel btmon capture.
set -u
MAC="${GANYMEDE_MAC:-00:A0:50:XX:XX:XX}"

bluetoothctl <<'EOF' >/dev/null 2>&1
agent NoInputNoOutput
default-agent
power on
EOF
bluetoothctl disconnect "$MAC" >/dev/null 2>&1
bluetoothctl remove "$MAC"     >/dev/null 2>&1
sleep 1

echo ">>> HOLD the remote in PAIRING MODE for the entire run <<<"
# Persistent active scan in the background (kept on the whole time).
( bluetoothctl scan on >/tmp/scan.out 2>&1 ) &
SCAN_BG=$!
# wait until the device object exists
for i in $(seq 1 15); do
  bluetoothctl info "$MAC" >/dev/null 2>&1 && { echo "device visible after ${i}s"; break; }
  sleep 1
done

ok=0
for i in $(seq 1 10); do
  echo "--- pair attempt $i ---"
  out=$(timeout 25 bluetoothctl pair "$MAC" 2>&1)
  echo "$out" | grep -iE "Pairing successful|Failed|Authentication|not available|AlreadyExists|Connected:|le-connection" | head -4
  if echo "$out" | grep -qiE "Pairing successful|AlreadyExists"; then ok=1; break; fi
  sleep 1
done

kill "$SCAN_BG" >/dev/null 2>&1
bluetoothctl scan off >/dev/null 2>&1

echo "RESULT ok=$ok"
echo "=== info ==="; bluetoothctl info "$MAC" 2>/dev/null
if [ "$ok" -eq 1 ]; then
  bluetoothctl trust "$MAC" >/dev/null 2>&1
  for i in $(seq 1 20); do bluetoothctl info "$MAC" 2>/dev/null | grep -qi "ServicesResolved: yes" && break; sleep 1; done
  echo "=== GATT ==="; bluetoothctl gatt.list-attributes "$MAC" 2>/dev/null
fi
