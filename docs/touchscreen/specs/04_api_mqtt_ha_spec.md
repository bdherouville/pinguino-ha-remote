# API, MQTT and Home Assistant Specification

## HTTP API compatibility

Preserve existing endpoints:

```txt
GET  /
GET  /api/status
GET  /api/scan
POST /api/connect
POST /api/mqtt
POST /api/press
```

Do not break existing Home Assistant or script users.

## Button press API

`POST /api/press`

Request:

```json
{
  "button": "power"
}
```

Valid values:

```txt
power
up
down
mode
eco
timer
fan
silent
flap
```

Response success:

```json
{
  "ok": true,
  "button": "power",
  "queued": true
}
```

Response unavailable:

```json
{
  "ok": false,
  "error": "nrf_unavailable"
}
```

## Status API

Extend `/api/status` with display and BME680 state.

Response shape:

```json
{
  "firmware": {
    "name": "pinguino-ha-remote-touch",
    "version": "0.1.0",
    "board": "esp32_touchscreen"
  },
  "wifi": {
    "connected": true,
    "ssid": "example",
    "ip": "192.168.1.50",
    "rssi": -55
  },
  "mqtt": {
    "configured": true,
    "connected": true,
    "host": "192.168.1.10"
  },
  "nrf": {
    "available": true,
    "status": "ready",
    "last_seen_ms": 420,
    "last_message": "ready"
  },
  "sensor": {
    "type": "BME680",
    "available": true,
    "temperature_c": 23.4,
    "humidity_percent": 51.2,
    "pressure_hpa": 1014.2,
    "gas_resistance_ohm": 82341,
    "air_quality": "Good",
    "last_update_ms": 3000
  },
  "display": {
    "available": true,
    "touch_available": true,
    "brightness_percent": 80,
    "backlight_on": true,
    "screen": "remote",
    "theme": "dark"
  }
}
```

## MQTT command topics

Preserve command model:

```txt
ganymede/cmd/<button>
```

Payload may be empty or `PRESS`.

Buttons:

```txt
ganymede/cmd/power
ganymede/cmd/up
ganymede/cmd/down
ganymede/cmd/mode
ganymede/cmd/eco
ganymede/cmd/timer
ganymede/cmd/fan
ganymede/cmd/silent
ganymede/cmd/flap
```

## MQTT state topics

Required:

```txt
ganymede/state/availability
ganymede/state/nrf_status
ganymede/state/last_button
ganymede/state/sensor/temperature
ganymede/state/sensor/humidity
ganymede/state/sensor/pressure
ganymede/state/sensor/gas_resistance
ganymede/state/sensor/air_quality
ganymede/state/display/brightness
ganymede/state/wifi/rssi
```

## Home Assistant discovery

Expose:

### Buttons

- Power
- Temperature Up
- Temperature Down
- Mode
- Fan
- Silent
- Eco
- Timer
- Flap

### Sensors

- Temperature
- Humidity
- Pressure
- Gas resistance
- Air quality
- Wi-Fi RSSI
- nRF status
- Firmware version

### Numbers / selects

- Display brightness
- Screen timeout
- Theme selector if practical

## Availability

Use one primary availability topic:

```txt
ganymede/state/availability
```

Values:

```txt
online
offline
```

For nRF availability, use a separate diagnostic sensor rather than marking the whole ESP32 unavailable.
