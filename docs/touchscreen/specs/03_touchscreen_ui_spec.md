# Touchscreen UI Specification

## Framework

Use LVGL.

The UI must be event-driven and must not block Wi-Fi, MQTT, HTTP, UART, or sensor polling.

## Global UI rules

- Default screen: Remote
- Touch wakes display
- Backlight timeout default: 60 seconds
- Brightness default: 80%
- Dark theme default
- All button presses must provide immediate visual feedback
- Disabled buttons must look disabled
- UI must tolerate missing Wi-Fi, MQTT, nRF, or BME680

## Screen 1 — Remote

Purpose: daily AC control.

Required visible elements:

- title: `Pinguino`
- nRF status badge
- Wi-Fi badge
- MQTT badge
- ambient temperature
- humidity
- air quality indicator

Required controls:

- Power
- Temp Up
- Temp Down
- Mode
- Fan
- Silent
- Eco
- Timer
- Flap

Button behavior:

```txt
Touch button -> bridge_press_button(button_name) -> command queue -> UART -> nRF52840
```

The UI must never write directly to UART.

Remote buttons are enabled only when:

```txt
nRF status == ready OR nRF status == bonded
```

If nRF is offline, show:

```txt
nRF offline — commands disabled
```

## Screen 2 — Status

Required fields:

- firmware version
- board name
- IP address
- SSID
- Wi-Fi RSSI
- MQTT host
- MQTT connection state
- nRF status
- nRF last seen age
- last command
- last command timestamp
- BME680 status
- uptime
- free heap

## Screen 3 — Sensors

Required fields:

- temperature in °C
- humidity in %
- pressure in hPa
- gas resistance in Ω
- air quality text
- last sensor update age

Air quality values:

```txt
Good
Average
Poor
Unknown
```

No medical or health claim. Label it as a relative indoor-air indicator only.

## Screen 4 — Settings

Required controls:

- brightness slider
- screen timeout selector
- theme selector: dark/light
- reboot button
- reset Wi-Fi provisioning button
- show web configuration URL

MQTT configuration remains web-based.

## Navigation

Use either:

- bottom tab bar, or
- swipe navigation with persistent home/back affordance

Remote screen must be reachable in one tap from any screen.

## Rendering constraints

- Avoid tiny text.
- Minimum touch target: 44 x 44 px.
- Status badges must be readable at arm’s length.
- Main controls must be usable without precision tapping.

## Error states

Display clear user messages:

```txt
Wi-Fi disconnected
MQTT disconnected
nRF offline
BME680 unavailable
Touch unavailable
Display degraded mode
```
