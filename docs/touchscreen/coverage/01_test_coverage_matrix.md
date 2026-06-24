# Test Coverage Matrix

| Area | Requirement | Unit | Integration | HIL | Manual |
|---|---|---:|---:|---:|---:|
| Commands | Valid button list | yes | yes | yes | yes |
| Commands | Invalid button rejected | yes | yes | no | no |
| Commands | HTTP uses command queue | no | yes | yes | yes |
| Commands | MQTT uses command queue | no | yes | yes | yes |
| Commands | UI uses command queue | no | yes | yes | yes |
| UART | Correct `press <button>\n` format | yes | yes | yes | no |
| UART | nRF status parsing | yes | yes | yes | no |
| UART | nRF timeout disables commands | yes | yes | yes | yes |
| State | Mutex-protected snapshot | yes | yes | no | no |
| State | Last command stored | yes | yes | yes | yes |
| BME680 | Detect `0x76` | no | yes | yes | no |
| BME680 | Detect `0x77` | no | yes | yes | no |
| BME680 | Missing sensor tolerated | yes | yes | yes | yes |
| BME680 | Stale data unavailable | yes | yes | yes | yes |
| BME680 | Gas resistance published | yes | yes | yes | yes |
| HTTP | `/api/status` schema | yes | yes | yes | no |
| HTTP | Secrets excluded | yes | yes | no | no |
| MQTT | Command subscription | no | yes | yes | no |
| MQTT | State publication | no | yes | yes | no |
| MQTT | Availability topic | no | yes | yes | no |
| HA | Button discovery | no | yes | yes | yes |
| HA | Sensor discovery | no | yes | yes | yes |
| UI | Remote screen renders | no | no | yes | yes |
| UI | Button feedback | no | no | yes | yes |
| UI | Disabled state | no | no | yes | yes |
| UI | Status screen renders | no | no | yes | yes |
| UI | Sensor screen renders | no | no | yes | yes |
| UI | Settings screen renders | no | no | yes | yes |
| Display | Brightness control | no | no | yes | yes |
| Display | Timeout and wake | no | no | yes | yes |
| Persistence | Brightness survives reboot | no | yes | yes | yes |
| Persistence | Theme survives reboot | no | yes | yes | yes |
| Resilience | Wi-Fi disconnect tolerated | no | yes | yes | yes |
| Resilience | MQTT disconnect tolerated | no | yes | yes | yes |
| Resilience | nRF missing tolerated | no | yes | yes | yes |
| Resilience | BME680 missing tolerated | no | yes | yes | yes |
