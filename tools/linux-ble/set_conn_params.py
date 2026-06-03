#!/usr/bin/env python3
"""Load per-device LE connection parameters into the kernel via the BlueZ
management (mgmt) socket — scoped to a single peer address only.

This sets the slow connection interval + long supervision timeout that the
Ganymede remote needs (it advertises connectable but is an ultra-low-power
sleeper that won't service BlueZ's default fast 45ms / 420ms link, dropping
with HCI reason 0x3e "Connection Failed to be Established").

Per-device only, runtime only (cleared on reboot / adapter reset). Needs root
(CAP_NET_ADMIN) for the mgmt CONTROL channel. Multiple mgmt clients may coexist
with bluetoothd, so no daemon restart is required.

mgmt-api.txt: Load Connection Parameters (0x0035)
  Param_Count(2) then per entry:
    Address(6) Address_Type(1) MinInterval(2) MaxInterval(2) Latency(2) Timeout(2)
  Address_Type: 0x01 = LE Public, 0x02 = LE Random
  Interval units = 1.25 ms, Timeout units = 10 ms
"""
import os
import socket
import struct
import sys

AF_BLUETOOTH = 31
BTPROTO_HCI = 1
HCI_CHANNEL_CONTROL = 3
HCI_DEV_NONE = 0xFFFF

MGMT_OP_LOAD_CONN_PARAM = 0x0035
MGMT_EV_CMD_COMPLETE = 0x0001
MGMT_EV_CMD_STATUS = 0x0002

TARGET = os.environ.get("GANYMEDE_MAC", "00:A0:50:XX:XX:XX")
ADDR_TYPE_LE_PUBLIC = 0x01

# Slow params: widen the ~6-event establishment window and protect the link.
# MATCH ANDROID'S SUCCESSFUL LINK (from logs/ganymede-btsnoop3.log): the remote
# only services FAST connection events (~48ms); slow intervals get 0 data. The
# critical fix vs BlueZ's default is the LONG 5s supervision timeout so a link
# that does establish survives the SMP/GATT round-trips. Most attempts still get
# 0x3e (short 6-event establishment window) — just retry persistently.
MIN_INTERVAL = 24    # *1.25ms = 30 ms
MAX_INTERVAL = 40    # *1.25ms = 50 ms  (Android negotiated 48.75ms)
LATENCY = 0
SUPERVISION_TIMEOUT = 500   # *10ms = 5000 ms (Android used 5000ms)


def addr_to_bytes(addr: str) -> bytes:
    return bytes(int(x, 16) for x in addr.split(":"))[::-1]  # mgmt wants LE byte order


def main(index: int):
    s = socket.socket(AF_BLUETOOTH, socket.SOCK_RAW, BTPROTO_HCI)
    s.bind((HCI_DEV_NONE, HCI_CHANNEL_CONTROL))
    s.settimeout(3.0)

    entry = (
        addr_to_bytes(TARGET)
        + struct.pack("<B", ADDR_TYPE_LE_PUBLIC)
        + struct.pack("<HHHH", MIN_INTERVAL, MAX_INTERVAL, LATENCY, SUPERVISION_TIMEOUT)
    )
    params = struct.pack("<H", 1) + entry  # param_count = 1
    hdr = struct.pack("<HHH", MGMT_OP_LOAD_CONN_PARAM, index, len(params))
    s.send(hdr + params)
    print(f"Sent Load Connection Parameters for {TARGET} on hci{index}:")
    print(f"  interval {MIN_INTERVAL*1.25:.0f}-{MAX_INTERVAL*1.25:.0f} ms, "
          f"latency {LATENCY}, supervision {SUPERVISION_TIMEOUT*10} ms")

    # Read events until we see the command complete/status for our opcode.
    for _ in range(20):
        try:
            data = s.recv(1024)
        except socket.timeout:
            print("Timed out waiting for mgmt response.")
            return 1
        ev, idx, plen = struct.unpack("<HHH", data[:6])
        body = data[6:6 + plen]
        if ev == MGMT_EV_CMD_COMPLETE and len(body) >= 3:
            op, status = struct.unpack("<HB", body[:3])
            if op == MGMT_OP_LOAD_CONN_PARAM:
                print(f"Command Complete: status=0x{status:02x} "
                      f"({'success' if status == 0 else 'ERROR'})")
                return 0 if status == 0 else 2
        elif ev == MGMT_EV_CMD_STATUS and len(body) >= 3:
            op, status = struct.unpack("<HB", body[:3])
            if op == MGMT_OP_LOAD_CONN_PARAM:
                print(f"Command Status: status=0x{status:02x}")
                if status != 0:
                    return 2
    print("No matching response seen.")
    return 1


if __name__ == "__main__":
    idx = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    sys.exit(main(idx))
