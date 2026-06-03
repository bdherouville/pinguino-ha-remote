# SPEC — ESP32-S3 BLE Remote Scanner for Ganymede AC Remote

Status note, 2026-06-01: this is the historical BLE central/probe spec. The
HID report map and button reports have since been captured from Android HCI
snoop logs, and the current work is ESP32-S3 BLE HID peripheral emulation plus
code factorization. See `docs/esp-idf-factorization-plan.md`.

## Objective

Build an ESP32-S3 firmware that investigates a BLE air-conditioner remote named `Ganymede`, address observed as `00:A0:50:XX:XX:XX`.

The firmware must act as a BLE central/client. It must scan, connect, discover services, read all accessible characteristics/descriptors, enable notifications/indications where possible, and log all BLE activity in a machine-readable format.

This is **not** a passive BLE sniffer. ESP32-S3 cannot reliably capture arbitrary encrypted BLE traffic between the remote and the air-conditioner. The firmware is an active BLE GATT exploration tool.

## Known facts from nRF Connect logs

Observed device:

```text
Name: Ganymede
Address: 00:A0:50:XX:XX:XX
Appearance: 961 / Keyboard HID subtype
```

Observed services:

```text
0x1800 Generic Access
0x1801 Generic Attribute
0x180A Device Information
0x180F Battery Service
0x181A Environmental Sensing
0x1812 Human Interface Device
```

Important characteristics:

```text
0x2A00 Device Name                         R
0x2A01 Appearance                          R
0x2A04 Peripheral Preferred Conn Params    R
0x2A05 Service Changed                     I
0x2A19 Battery Level                       R
0x2A6F Humidity                            R N
0x2A6E Temperature                         R N
0x2A6D Pressure                            R N
0x2A7D Descriptor Value Changed            I
0x2A4B HID Report Map                      R
0x2A4D HID Report                          R W N / R W WNR depending instance
0x2A4A HID Information                     R
0x2A4C HID Control Point                   WNR
0x2A4E HID Protocol Mode                   R WNR
0x2A22 Boot Keyboard Input Report          R N
0x2A32 Boot Keyboard Output Report         R W WNR
```

Known observations:

```text
0x2A22 Boot Keyboard Input Report reads as:
00 00 00 00 00 00 00 00

0x2A32 Boot Keyboard Output Report reads as:
00

0x2A4E Protocol Mode reads as:
01 = Report Protocol Mode
```

Environmental notifications are visible:

```text
0x2A6E Temperature notifications
0x2A6F Humidity notifications
```

Android/nRF Connect cannot access the useful HID fields:

```text
0x2A4B Report Map
0x2A4D Report
0x2908 Report Reference

Error:
BLUETOOTH_PRIVILEGED permission required
```

## Hypothesis

The air-conditioner remote likely acts as a BLE peripheral/server. The air-conditioner probably acts as a BLE central/client and subscribes to reports exposed by the remote.

The useful command path is likely:

```text
HID Service 0x1812
  -> Report Map 0x2A4B
  -> Report 0x2A4D
  -> Report Reference 0x2908
```

The `Boot Keyboard Input Report 0x2A22` appears unused or only provides a zero idle report.

## Firmware requirements

### Platform

Target board:

```text
ESP32-S3
Framework: ESP-IDF
Bluetooth stack: NimBLE preferred
```

Do not use Arduino BLE libraries for the first implementation. Use ESP-IDF + NimBLE for better control over GATT discovery, descriptors, subscriptions, security, and logging.

### BLE roles

Firmware must operate as:

```text
BLE Central
BLE GATT Client
```

Optional later mode:

```text
BLE Peripheral / GATT Server emulator
```

Do not implement emulation in v1. The first milestone is full active discovery and logging.

## Functional requirements

### 1. Scanner

The firmware must scan for BLE advertisements and log:

```text
timestamp_ms
address
address_type
RSSI
advertising_type
device_name
service_uuids
manufacturer_data
service_data
appearance
raw_adv_hex
raw_scan_response_hex
```

It must support filtering by:

```text
device name = Ganymede
address = 00:A0:50:XX:XX:XX
service UUID = 0x1812
```

### 2. Connection

When the target is found, the firmware must connect as central.

Connection configuration should initially try:

```text
interval_min: 7.5 ms
interval_max: 30 ms
latency: 0
supervision_timeout: 5000 ms
```

Then log any negotiated connection parameter update.

The logs show the remote advertises preferred connection parameters equivalent to:

```text
Connection interval min/max: 4000 ms
Latency: 0
Supervision timeout multiplier: 3000
```

The firmware must log the raw value of characteristic `0x2A04`.

### 3. Service discovery

After connection, discover all primary services and characteristics.

For each service, log:

```text
service_uuid
start_handle
end_handle
```

For each characteristic, log:

```text
characteristic_uuid
declaration_handle
value_handle
properties
descriptors
```

Properties must be decoded:

```text
READ
WRITE
WRITE_NO_RESPONSE
NOTIFY
INDICATE
```

### 4. Descriptor discovery

For every characteristic, discover all descriptors.

Special descriptors to log and read when possible:

```text
0x2901 Characteristic User Description
0x2902 Client Characteristic Configuration
0x2906 Valid Range
0x2908 Report Reference
0x290C Environmental Sensing Measurement
0x290D Environmental Sensing Trigger Setting
```

For each descriptor, log:

```text
descriptor_uuid
handle
read_status
raw_value_hex
decoded_value_if_known
```

### 5. Read accessible characteristics

Read all readable characteristics.

Known characteristics to explicitly read:

```text
0x2A00 Device Name
0x2A01 Appearance
0x2A04 Peripheral Preferred Connection Parameters
0x2A19 Battery Level
0x2A6F Humidity
0x2A6E Temperature
0x2A6D Pressure
0x2A4B HID Report Map
0x2A4D HID Report
0x2A4A HID Information
0x2A4E Protocol Mode
0x2A22 Boot Keyboard Input Report
0x2A32 Boot Keyboard Output Report
```

For each read, log:

```text
uuid
handle
status
raw_value_hex
decoded_value_if_known
```

Important: even if Android failed with `BLUETOOTH_PRIVILEGED`, ESP32 must still attempt to read `0x2A4B`, `0x2A4D`, and `0x2908`. The failure may be Android-specific.

### 6. Enable notifications and indications

For every characteristic with NOTIFY or INDICATE:

1. call local subscription API;
2. write CCCD `0x2902`;
3. log subscription result;
4. log all notifications/indications.

Subscription values:

```text
Notify:   01 00
Indicate: 02 00
Both:     03 00
Disable:  00 00
```

Explicitly subscribe to:

```text
0x2A05 Service Changed                  indication
0x2A7D Descriptor Value Changed         indication
0x2A6F Humidity                         notification
0x2A6E Temperature                      notification
0x2A6D Pressure                         notification
0x2A4D HID Report                       notification
0x2A22 Boot Keyboard Input Report       notification
```

Every notification must be logged as:

```json
{
  "type": "notification",
  "timestamp_ms": 123456,
  "uuid": "00002a4d-0000-1000-8000-00805f9b34fb",
  "handle": 42,
  "value_hex": "..."
}
```

### 7. Button capture mode

Add a serial CLI command:

```text
capture-buttons
```

When enabled, the firmware should:

```text
- keep connection alive
- keep HID and Boot Keyboard notifications enabled
- print a visible marker every 5 seconds
- log all notifications
- log repeated reads of 0x2A22 and 0x2A4D every 500 ms for 60 seconds
```

Goal: press every physical button on the remote and detect whether any characteristic changes.

### 8. Security and pairing

Implement BLE security support.

The firmware must log:

```text
pairing requested
bonding result
encryption state
authentication status
MITM status
key distribution
security failure reason
```

Try these modes in order:

```text
Mode A: no bonding, no MITM
Mode B: bonding, no MITM
Mode C: bonding + MITM if passkey requested
```

Expose serial CLI commands:

```text
pair
bond
clear-bonds
connect
disconnect
security-status
```

If a read/subscribe fails before encryption, retry after pairing/bonding.

### 9. Logging

Output must be line-delimited JSON over USB serial.

Example:

```json
{"type":"scan","ts":1234,"addr":"00:A0:50:XX:XX:XX","rssi":-61,"name":"Ganymede","adv_hex":"..."}
{"type":"connect","ts":2345,"addr":"00:A0:50:XX:XX:XX","status":"ok"}
{"type":"service","uuid":"00001812-0000-1000-8000-00805f9b34fb","start":37,"end":55}
{"type":"char","service":"00001812-0000-1000-8000-00805f9b34fb","uuid":"00002a4d-0000-1000-8000-00805f9b34fb","handle":44,"props":["read","write","notify"]}
{"type":"read","uuid":"00002a22-0000-1000-8000-00805f9b34fb","status":"ok","value_hex":"0000000000000000"}
{"type":"subscribe","uuid":"00002a4d-0000-1000-8000-00805f9b34fb","status":"ok"}
{"type":"notification","uuid":"00002a6e-0000-1000-8000-00805f9b34fb","value_hex":"ae0a","decoded":"27.34 C"}
```

Also support a compact human-readable debug mode, but JSON must be the default.

### 10. Decoding

Implement decoders for:

```text
0x2A00 Device Name: UTF-8 string
0x2A01 Appearance: uint16 little-endian
0x2A04 Peripheral Preferred Connection Parameters
0x2A19 Battery Level: uint8 percent
0x2A6E Temperature: sint16 little-endian / 100 Celsius
0x2A6F Humidity: uint16 little-endian / 100 percent
0x2A6D Pressure: uint32 little-endian / 10 Pascal, if standard
0x2A4E Protocol Mode: 0 = Boot, 1 = Report
0x2A22 Boot Keyboard Input: 8-byte HID keyboard report
```

Do not guess HID report semantics for `0x2A4D` without the Report Map.

### 11. Serial CLI

Implement commands:

```text
help
scan
scan-stop
connect <addr>
disconnect
discover
read-all
read <uuid-or-handle>
read-desc <handle>
subscribe <uuid-or-handle>
unsubscribe <uuid-or-handle>
capture-buttons
pair
clear-bonds
security-status
dump-handles
set-target <addr>
reboot
```

### 12. Acceptance criteria

The firmware is acceptable when it can:

1. scan and identify `Ganymede`;
2. connect to `00:A0:50:XX:XX:XX`;
3. discover the same services observed in nRF Connect;
4. read battery, temperature, humidity, pressure, protocol mode, and boot keyboard report;
5. subscribe to temperature and humidity notifications;
6. attempt to read `0x2A4B`, `0x2A4D`, and `0x2908`;
7. log whether these HID fields are accessible from ESP32;
8. run `capture-buttons` and produce a complete JSON log while buttons are pressed;
9. support pairing/bonding and retry protected reads after encryption.

### 13. Non-goals for v1

Do not implement:

```text
- BLE passive sniffing
- Wireshark-compatible capture
- HID emulation
- AC command replay
- Home Assistant integration
- MQTT
- IR/RF support
```

These are later phases.

### 14. Project structure

Use this layout:

```text
.
├── CMakeLists.txt
├── sdkconfig.defaults
├── README.md
├── main
│   ├── CMakeLists.txt
│   ├── main.c
│   ├── ble_scan.c
│   ├── ble_scan.h
│   ├── ble_client.c
│   ├── ble_client.h
│   ├── ble_security.c
│   ├── ble_security.h
│   ├── gatt_decode.c
│   ├── gatt_decode.h
│   ├── logger.c
│   ├── logger.h
│   ├── cli.c
│   └── cli.h
└── tools
    └── serial_capture.py
```

### 15. README requirements

README must explain:

```text
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py flash monitor
```

It must document:

```text
- how to scan
- how to connect
- how to read all characteristics
- how to enable notifications
- how to run capture-buttons
- how to save logs
- known limitation: ESP32-S3 active GATT client, not passive BLE sniffer
```

### 16. Safety constraints

The firmware must not spam writes to the remote.

Allowed writes in v1:

```text
- CCCD writes 0x2902 for notifications/indications
- Protocol Mode write only if explicitly requested by CLI
- HID Control Point write only if explicitly requested by CLI
```

Default behavior must be read-only plus notification subscription.

### 17. Key investigation questions

The firmware must help answer:

```text
Can ESP32 read the HID Report Map 0x2A4B?
Can ESP32 read the HID Report Reference 0x2908?
Can ESP32 subscribe to HID Report 0x2A4D?
Do button presses produce notifications on 0x2A4D?
Do button presses affect 0x2A22?
Is pairing/encryption required before HID reports become visible?
Does the remote expose hidden/protected attributes not visible from Android?
```
