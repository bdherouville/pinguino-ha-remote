# Code Assessment Process

This process exists to keep the touchscreen firmware improving without
reintroducing failures already found during bring-up: wrong chip/flash target,
blocking LVGL work, weak touch wake behavior, UI regressions, lost NVS data, and
command paths bypassing the queue.

## Change rules

Every firmware change must be assessed against these rules before it is flashed,
merged, or released.

### 1. Contract preservation

- Keep HTTP, MQTT, Home Assistant discovery, UART wire protocol, and button names
  backward compatible unless a deliberate migration is documented.
- All button sources must still go through `bridge_press_button()` or
  `bridge_press_button_wait()`. UI, HTTP and MQTT must not write UART directly.
- Secrets must not appear in `/api/status`, logs, screenshots, test fixtures or
  reports.
- Per-device configuration must stay in NVS, not inside the app partition.

### 2. Hardware and partition correctness

- Verify the connected chip before flashing. The touchscreen board is plain
  ESP32; the headless board is ESP32-S3.
- Keep board-specific GPIO and driver details in `board_config.*` or `pins.*`.
- Do not erase flash for normal firmware updates. Flash bootloader, partition
  table and app only, preserving NVS unless the task explicitly requires a wipe.
- For the current 4 MB touchscreen board, preserve the custom partition layout:
  NVS at `0x9000`, PHY at `0xf000`, factory app at `0x10000`.

### 3. LVGL responsiveness

- Do not perform long-running work in LVGL event callbacks, display flush, touch
  read, or the UI task loop.
- Any page that can become partially visible during a swipe must already have
  its objects created, or must be prepared at scroll start before it is visible.
- Avoid rebuilding unchanged labels, widgets and styles on every refresh.
- Keep touch targets at least 44 x 44 px and keep touch feedback visible for
  press and queued-command states.
- Screen timeout must wake from raw touch activity, even when the backlight is
  off.

### 4. Watchdog and task behavior

- Treat any `task_wdt` boot or interaction log as a release blocker.
- UI work must yield often enough for both idle tasks to run.
- Hardware probing must have bounded timeouts. Missing nRF52840, BME680, MQTT or
  Wi-Fi must not prevent local touchscreen boot.
- Do not hold mutexes during I/O or delays.

### 5. Test coverage

- Source-level contract tests must cover each rule that can be checked without
  hardware.
- Add or update tests when changing button catalogs, command routing, partition
  settings, board definitions, status schemas, MQTT/HA discovery, LVGL lifecycle,
  or persistence behavior.
- Hardware-in-loop checks are required for display/touch/backlight changes,
  wake behavior, boot watchdog fixes, flash layout changes and swipe/navigation
  changes.
- Manual acceptance is required for visual changes to controls, animation,
  brightness, disabled states and page transitions.

### 6. Optimization standards

- First reduce work; then tune scheduling; only then add configuration-level
  performance changes.
- Prefer incremental UI updates (`label_set_text_if_changed` style) over
  unconditional redraws.
- Keep absent-device probes short and explicit.
- Do not add background polling faster than the UI, network or sensor path can
  justify.
- Track binary size after each build and keep meaningful free space in the app
  partition.

## Local assessment command

Run the default local assessment before flashing or asking for review:

```bash
python3 tools/assess_touchscreen_change.py
```

For firmware work, also build the touched flavor:

```bash
python3 tools/assess_touchscreen_change.py --idf-build touchscreen
```

For release-risk work, build both flavors:

```bash
python3 tools/assess_touchscreen_change.py --idf-build all
```

For hardware-in-loop boot validation after flashing:

```bash
python3 tools/assess_touchscreen_change.py --port /dev/ttyUSB0 --boot-log
```

The script runs the automated local gates and prints the human checklist. A
change is not ready until the automated gates pass and each relevant manual item
has either been tested or explicitly marked not applicable in the review notes.

## Review checklist

Use this checklist in code reviews and local self-assessments.

- Scope: change is limited to the requested behavior and does not mix unrelated
  refactors.
- Contracts: APIs, MQTT topics, HA discovery, UART strings and NVS keys are
  unchanged or deliberately migrated.
- Tests: every changed contract has a source-level test or a documented reason
  it needs HIL/manual coverage.
- Hardware: chip target, flash size, partition offsets and GPIO ownership are
  correct for each flavor.
- Responsiveness: LVGL callbacks and task loops do not contain blocking work;
  swipe and wake paths are tested on hardware when touched.
- Resilience: missing nRF, BME680, MQTT and Wi-Fi paths still boot and degrade
  visibly.
- Observability: logs identify boot phase, board config, display/touch init,
  nRF state and connection changes without steady-state noise.
- Security: no Wi-Fi/MQTT secrets are logged, exposed or committed.
- Release: build output size is recorded and the relevant flavor has been
  flashed/boot-checked for firmware changes.

## Reference sources

- ESP-IDF v6.0.1 Watchdogs:
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/wdts.html
- ESP-IDF v6.0.1 Partition Tables:
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/partition-tables.html
- ESP-IDF v6.0.1 NVS:
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/nvs_flash.html
- Espressif ESP-BSP LCD/LVGL performance notes:
  https://github.com/espressif/esp-bsp/blob/master/components/esp_lvgl_port/docs/performance.md
- LVGL documentation:
  https://lvgl.io/docs/open/
