#!/usr/bin/env python3
"""Static invariants for the ESP32 bridge touchscreen flavor.

These checks are intentionally small and dependency-free. They do not replace
ESP-IDF builds or hardware tests; they catch spec regressions that compile cleanly,
such as bypassing the command queue or dropping one release flavor.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "esp32_bridge"
MAIN = FW / "main"

EXPECTED_BUTTONS = {
    "BRIDGE_BUTTON_POWER": "power",
    "BRIDGE_BUTTON_UP": "up",
    "BRIDGE_BUTTON_DOWN": "down",
    "BRIDGE_BUTTON_MODE": "mode",
    "BRIDGE_BUTTON_FAN": "fan",
    "BRIDGE_BUTTON_SILENT": "silent",
    "BRIDGE_BUTTON_ECO": "eco",
    "BRIDGE_BUTTON_TIMER": "timer",
    "BRIDGE_BUTTON_FLAP": "flap",
}


failures: list[str] = []


def strip_c_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//.*", "", text)
    return text


def read(path: Path | str) -> str:
    p = ROOT / path if isinstance(path, str) else path
    try:
        return p.read_text(encoding="utf-8")
    except FileNotFoundError:
        failures.append(f"missing file: {p.relative_to(ROOT)}")
        return ""


def require(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


def require_contains(text: str, needle: str, message: str) -> None:
    require(needle in text, message)


def require_order(text: str, before: str, after: str, message: str) -> None:
    before_pos = text.find(before)
    after_pos = text.find(after)
    require(before_pos >= 0 and after_pos >= 0 and before_pos < after_pos, message)


def check_flavor_configs() -> None:
    common = read(FW / "sdkconfig.defaults")
    headless = read(FW / "sdkconfig.defaults.headless")
    touchscreen = read(FW / "sdkconfig.defaults.touchscreen")
    kconfig = read(MAIN / "Kconfig.projbuild")

    require('CONFIG_IDF_TARGET=' not in common, "common sdkconfig.defaults must not force one IDF target")
    require_contains(headless, 'CONFIG_IDF_TARGET="esp32s3"', "headless flavor must target ESP32-S3")
    require_contains(touchscreen, 'CONFIG_IDF_TARGET="esp32"', "touchscreen flavor must target plain ESP32")
    require_contains(touchscreen, "CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN=y", "touchscreen flavor flag missing")
    require_contains(touchscreen, "CONFIG_I2CDEV_TIMEOUT=250", "touchscreen missing fast absent-BME680 I2C timeout")
    for font in ("14", "16", "20", "24"):
        require_contains(touchscreen, f"CONFIG_LV_FONT_MONTSERRAT_{font}=y", f"touchscreen missing Montserrat {font}")
    require_contains(kconfig, 'default "pinguino-ha-remote-touch"', "touchscreen firmware name missing")
    require_contains(kconfig, 'default "pinguino-ha-remote-headless"', "headless firmware name missing")


def check_ci_and_release_matrices() -> None:
    ci = read(ROOT / ".github" / "workflows" / "ci.yml")
    release = read(ROOT / ".github" / "workflows" / "release.yml")
    flash_manifest = read(ROOT / "docs" / "flash" / "manifest.json")
    flash_index = read(ROOT / "docs" / "flash" / "index.html")
    top_readme = read(ROOT / "README.md")

    for flavor, target in (("headless", "esp32s3"), ("touchscreen", "esp32")):
        require_contains(ci, f"flavor: {flavor}", f"CI missing {flavor} build matrix row")
        require_contains(ci, f"target: {target}", f"CI missing {target} target")
        require_contains(release, f"flavor: {flavor}", f"release missing {flavor} matrix row")
        require_contains(release, f"target: {target}", f"release missing {target} target")

    require_contains(release, "ganymede-bridge-headless-esp32s3.bin", "release missing headless image name")
    require_contains(release, "ganymede-bridge-touchscreen-esp32.bin", "release missing touchscreen image name")
    require_contains(release, "ganymede-bridge-esp32s3.bin", "release missing legacy headless image copy")
    require_contains(release, "bootloader_offset: '0x1000'", "release missing ESP32 bootloader offset")
    require_contains(release, "bootloader_offset: '0x0'", "release missing ESP32-S3 bootloader offset")
    require_contains(ci, "python3 tools/test_touchscreen_contracts.py", "CI must run touchscreen contract tests")
    require_contains(release, "python3 tools/test_touchscreen_contracts.py", "release must run touchscreen contract tests")
    require_contains(flash_manifest, '"chipFamily": "ESP32-S3"', "web flasher manifest missing ESP32-S3 build")
    require_contains(flash_manifest, '"chipFamily": "ESP32"', "web flasher manifest missing plain ESP32 build")
    require_contains(flash_manifest, "ganymede-bridge-headless-esp32s3.bin", "web flasher must use named headless image")
    require_contains(flash_manifest, "ganymede-bridge-touchscreen-esp32.bin", "web flasher missing touchscreen image")
    require_contains(flash_index, "plain ESP32 touchscreen", "web flasher page must explain touchscreen target")
    require_contains(top_readme, "ganymede-bridge-touchscreen-esp32.bin", "top-level README missing touchscreen release image")
    require_contains(top_readme, "touchscreen build targets a plain ESP32", "top-level README must say touchscreen is plain ESP32")


def check_board_abstraction() -> None:
    board_h = read(MAIN / "board_config.h")
    board_c = read(MAIN / "board_config.c")

    for token in (
        '#define BOARD_NAME "esp32_touchscreen"',
        '#define BOARD_NAME "esp32s3_headless"',
        '#define BOARD_MODEL "JC2432W328"',
        "#define NRF_UART_PORT UART_NUM_1",
        "#define NRF_UART_BAUDRATE 115200",
        "#define NRF_HEARTBEAT_GPIO -1",
        "#define BME680_I2C_FREQ_HZ BME_I2C_FREQ_HZ",
        '#define DISPLAY_DRIVER_NAME "ST7789"',
        '#define TOUCH_DRIVER_NAME "CST820"',
        "#define DISPLAY_H_RES 320",
        "#define DISPLAY_V_RES 240",
        "#define DISPLAY_SPI_MISO_GPIO 12",
        "#define DISPLAY_SPI_MOSI_GPIO 13",
        "#define DISPLAY_SPI_SCLK_GPIO 14",
        "#define DISPLAY_SPI_CS_GPIO 15",
        "#define DISPLAY_DC_GPIO 2",
        "#define DISPLAY_BACKLIGHT_GPIO 27",
        "#define TOUCH_I2C_ADDR_CST820 0x15",
        "#define TOUCH_RST_GPIO 25",
        "#define TOUCH_INT_GPIO 21",
        "#define BME_I2C_SDA_GPIO 33",
        "#define BME_I2C_SCL_GPIO 32",
        "#define LD2410_UART_TX_GPIO 19",
        "#define LD2410_UART_RX_GPIO 18",
        "esp_err_t board_display_init(void);",
        "esp_err_t board_touch_init(void);",
        "esp_err_t board_backlight_set(uint8_t percent);",
    ):
        require_contains(board_h, token, f"board_config.h missing {token}")

    for token in (
        "esp_lcd_new_panel_st7789",
        "LCD_RGB_ELEMENT_ORDER_BGR",
        ".spi_mode = 3",
        "lv_draw_sw_rgb565_swap(px_map, lv_area_get_size(area))",
        "esp_lcd_panel_mirror(s_lcd_panel, false, true)",
        "esp_lcd_panel_swap_xy(s_lcd_panel, true)",
        "*x = DISPLAY_H_RES - 1 - raw_y",
        "*y = raw_x",
        "CONFIG_PINGUINO_TOUCHSCREEN_DISPLAY_DIAGNOSTIC",
        "lv_display_create(DISPLAY_H_RES, DISPLAY_V_RES)",
        "lv_display_set_buffers",
        "lv_indev_create",
        "i2c_dev_check_present(&s_touch_dev)",
        "cst820_write(0xFE, 0xFF)",
        "ledc_set_duty",
        "ledc_update_duty",
    ):
        require_contains(board_c, token, f"JC2432W328 board driver missing {token}")

    uart = read(MAIN / "uart_link.c")
    require_contains(uart, "static int8_t s_hb_gpio = PIN_DEF_NRF_HB", "nRF heartbeat GPIO must support disabled -1")
    require_contains(uart, "s_hb_gpio >= 0 ? gpio_get_level(s_hb_gpio) : -1", "disabled heartbeat must not sample GPIO")
    require_contains(uart, "heartbeat disabled", "UART init must log disabled heartbeat")
    handle_line = uart.split("static void handle_line", 1)[-1].split("static void reflect", 1)[0]
    require_order(
        handle_line,
        'strncmp(line, "status ", 7)',
        "s_last_rx_us = esp_timer_get_time();",
        "floating UART noise must not refresh nRF liveness before status token validation",
    )
    require_contains(uart, "gpio_set_pull_mode((gpio_num_t)pn->nrf_rx, GPIO_PULLUP_ONLY)", "unwired nRF RX must be pulled to idle high")

    pins = read(MAIN / "pins.c")
    web = read(MAIN / "web.c")
    require_contains(pins, "nvs_get_i8", "pin NVS must support signed heartbeat disable value")
    require_contains(pins, "nvs_get_u8", "pin NVS must read legacy unsigned pin values")
    require_contains(pins, "nvs_set_i8", "pin NVS must persist signed pin values")
    require_contains(web, 'form_pin(body, "nrf_hb",  &p.nrf_hb,  true)', "web pin API must allow nrf_hb=-1")

    scattered_gpio_files = []
    for path in MAIN.glob("*.c"):
        if path.name in {"board_config.c", "pins.c"}:
            continue
        text = strip_c_comments(read(path))
        if re.search(r"\bGPIO_NUM_\d+\b|#define\s+\w*GPIO\w*\s+\d+", text):
            scattered_gpio_files.append(path.name)
    require(not scattered_gpio_files, f"possible hardcoded GPIOs outside board/pins: {', '.join(scattered_gpio_files)}")


def check_button_catalog() -> None:
    header = read(MAIN / "bridge_buttons.h")
    impl = read(MAIN / "bridge_buttons.c")
    uart = read(MAIN / "uart_link.c")

    found = dict(re.findall(r"#define\s+(BRIDGE_BUTTON_[A-Z_]+)\s+\"([a-z_]+)\"", header))
    require(found == EXPECTED_BUTTONS, f"button constants mismatch: {found!r}")
    for macro in EXPECTED_BUTTONS:
        require_contains(impl, "{" + macro + ",", f"central button table does not use {macro}")
    require("VALID[]" not in uart, "uart_link must not carry a second valid-button table")
    require_contains(uart, "return bridge_button_valid(btn);", "uart_link validation must use central button table")

    ac_cmd = strip_c_comments(read(MAIN / "ac_cmd.c"))
    for button in EXPECTED_BUTTONS.values():
        require(f'"{button}"' not in ac_cmd, f"ac_cmd.c must use BRIDGE_BUTTON_* instead of literal {button!r}")


def check_command_funnel() -> None:
    allowed_uart_press = {"uart_link.c", "uart_link.h", "command_queue.c"}
    allowed_uart_write = {"uart_link.c"}

    for path in MAIN.glob("*.c"):
        text = strip_c_comments(read(path))
        if "uart_link_press(" in text and path.name not in allowed_uart_press:
            failures.append(f"{path.name} calls uart_link_press directly; use bridge_press_button()")
        if "uart_write_bytes(" in text and path.name not in allowed_uart_write:
            failures.append(f"{path.name} writes UART directly; only uart_link.c may do that")

    command_queue = read(MAIN / "command_queue.c")
    require_contains(command_queue, "xQueueCreate", "command queue must allocate a FreeRTOS queue")
    require_contains(command_queue, "xQueueSend(s_queue", "bridge_press_button must enqueue commands")
    require_contains(command_queue, "uart_link_press(cmd.button)", "only command worker should write button commands")
    require_contains(command_queue, "bridge_state_update_last_command", "command worker must update last-command state")

    for path_name, expected in (
        ("web.c", "bridge_press_button(btn)"),
        ("mqtt_ha.c", "bridge_press_button(btn)"),
        ("ui_lvgl.c", "bridge_press_button(button_name)"),
        ("ac_cmd.c", "bridge_press_button_wait(b, &sent)"),
    ):
        text = read(MAIN / path_name)
        require_contains(text, expected, f"{path_name} must route button commands through command queue")

    ui = read(MAIN / "ui_lvgl.c")
    uart = read(MAIN / "uart_link.c")
    require_contains(
        uart,
        "return esp_timer_get_time() < s_mute_until_us ||\n           s_effective == NRF_READY;",
        "touch commands must model only during sync or once nRF is ready",
    )
    require_contains(ui, "uart_link_mute_secs() > 0 || st == NRF_READY", "touch UI must enable commands only for sync/ready nRF")


def check_state_api_and_sensors() -> None:
    state_h = read(MAIN / "bridge_state.h")
    state_c = read(MAIN / "bridge_state.c")
    web = read(MAIN / "web.c")
    bme = read(MAIN / "bme280.c")

    for field in (
        "wifi_connected",
        "mqtt_connected",
        "nrf_available",
        "sensor_available",
        "gas_resistance_ohm",
        "air_quality",
        "last_button",
        "display_available",
        "touch_available",
        "brightness_percent",
        "current_screen",
        "theme",
        "uptime_s",
        "free_heap",
    ):
        require_contains(state_h, field, f"bridge_state_t missing {field}")

    require_contains(state_c, "xSemaphoreCreateMutex", "bridge state must be mutex-protected")
    require_contains(state_c, "SENSOR_STALE_MS 120000ULL", "sensor stale timeout must be 120 seconds")
    require_contains(state_c, "out->sensor_available = false", "stale sensor snapshots must be unavailable")

    status_section = web.split("static esp_err_t h_status", 1)[-1].split("static esp_err_t h_scan", 1)[0]
    for key in (
        "firmware",
        "wifi",
        "mqtt_detail",
        "nrf_detail",
        "sensor",
        "display",
        "runtime",
        "gas_resistance_ohm",
        "air_quality",
        "screen_timeout_s",
    ):
        require_contains(status_section, f'\\"{key}\\"', f"/api/status missing {key}")
    require_contains(web, "static void json_escape_str", "web JSON responses must escape string values")
    require_contains(status_section, "const size_t buflen = 4096", "/api/status buffer should leave room for extended status schema")
    require_contains(status_section, "json_escape_str(st.wifi_ssid", "/api/status must escape Wi-Fi SSID")
    require_contains(status_section, "json_escape_str(st.mqtt_host", "/api/status must escape MQTT host")
    require_contains(status_section, "json_escape_str(st.nrf_last_message", "/api/status must escape nRF messages")
    require('"pass"' not in status_section and "s_pass" not in status_section, "/api/status must not expose passwords")

    for token in (
        "BME680_I2C_ADDR_0",
        "BME680_I2C_ADDR_1",
        "BME680_I2C_FREQ_HZ",
        "bme680_init_sensor",
        "bridge_state_update_sensor(false",
        "mqtt_ha_publish_env_ext",
        "air_quality_from_gas",
    ):
        require_contains(bme, token, f"BME680 implementation missing {token}")


def check_mqtt_and_ha_surface() -> None:
    mqtt = read(MAIN / "mqtt_ha.c")

    for token in (
        '#define STATE_AVTY_TOPIC AVTY_TOPIC',
        '#define CMD_PREFIX "ganymede/cmd/"',
        '#define NRF_STATE_TOPIC "ganymede/state/nrf_status"',
        '#define LAST_BUTTON_TOPIC "ganymede/state/last_button"',
        '#define SENSOR_STATE_BASE "ganymede/state/sensor/"',
        '#define DISPLAY_BRIGHTNESS_TOPIC "ganymede/state/display/brightness"',
        '#define WIFI_RSSI_TOPIC "ganymede/state/wifi/rssi"',
        "bridge_buttons(&nbuttons)",
        "homeassistant/button/ganymede_%s/config",
        '{"gas_resistance", "Gas resistance"',
        '{"air_quality", "Air quality"',
        "homeassistant/number/ganymede_display_brightness/config",
        "homeassistant/select/ganymede_display_theme/config",
        "esp_mqtt_client_subscribe(s_client, CMD_PREFIX \"+\", 1)",
        "bridge_state_update_mqtt",
    ):
        require_contains(mqtt, token, f"MQTT/HA surface missing {token}")

    for state_topic in (
        'SENSOR_STATE_BASE "temperature"',
        'SENSOR_STATE_BASE "humidity"',
        'SENSOR_STATE_BASE "pressure"',
        'SENSOR_STATE_BASE "gas_resistance"',
        'SENSOR_STATE_BASE "air_quality"',
        "WIFI_RSSI_TOPIC",
    ):
        require_contains(mqtt, state_topic, f"MQTT state publication missing {state_topic}")
    require_order(
        mqtt,
        "#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN\n    snprintf(topic, sizeof(topic), \"homeassistant/number/ganymede_display_brightness/config\")",
        "#endif\n    // LD2410 presence",
        "display HA discovery must be touchscreen-only",
    )
    require_order(
        mqtt,
        "#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN\n        esp_mqtt_client_subscribe(s_client, DISPLAY_BRIGHTNESS_CMD_TOPIC, 1);",
        "#endif\n        { ac_state_t snap;",
        "display MQTT command subscriptions must be touchscreen-only",
    )
    require_order(
        mqtt,
        "#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN\n        } else if (!strcmp(topic, DISPLAY_BRIGHTNESS_CMD_TOPIC))",
        "#endif\n        }\n        break;",
        "display MQTT command handling must be touchscreen-only",
    )


def check_ui_settings() -> None:
    ui = read(MAIN / "ui_lvgl.c")
    web = read(MAIN / "web.c")
    ui_start = ui.split("esp_err_t ui_lvgl_start(void)", 1)[-1].split("bool ui_lvgl_display_available", 1)[0]
    for token in (
        '#define UI_NVS_NS "ui"',
        "#define UI_TIMEOUT_DEFAULT_S 60",
        "static uint8_t s_brightness_percent = 100",
        'static char s_theme[8] = "dark"',
        'nvs_set_u8(h, "bright"',
        'nvs_set_u16(h, "timeout"',
        'nvs_set_str(h, "theme"',
        "board_backlight_set(0)",
        "lv_tileview_create",
        "lv_tileview_add_tile",
        "lv_tileview_set_tile_by_index",
        "lv_tileview_get_tile_active",
        "lv_obj_set_scrollbar_mode",
        "LV_SCROLLBAR_MODE_OFF",
        "screen_index_for_name",
        "ui_lvgl_note_activity",
        "activity_event",
        "LV_EVENT_PRESSED",
        "Queued: %s",
        "wifi_mgr_reset_provisioning",
        "lv_timer_handler",
    ):
        require_contains(ui, token, f"UI settings/behavior missing {token}")
    require_order(
        ui_start,
        "lv_init();",
        "esp_err_t display = board_display_init();",
        "LVGL must be initialized before board_display_init registers a real panel",
    )
    require_order(
        ui_start,
        "esp_err_t display = board_display_init();",
        "init_lvgl_screens();",
        "UI screen setup must wait until the board layer has registered a display",
    )

    h_ui = web.split("static esp_err_t h_ui", 1)[-1].split("static esp_err_t h_pairing", 1)[0]
    for token in (
        'json_value_get(body, "screen"',
        'json_value_get(body, "brightness"',
        'json_value_get(body, "timeout"',
        'json_value_get(body, "theme"',
        'json_value_get(body, "wake"',
        'json_string_get(body, "button"',
    ):
        require_contains(h_ui, token, f"/api/ui missing JSON support for {token}")


def main() -> int:
    check_flavor_configs()
    check_ci_and_release_matrices()
    check_board_abstraction()
    check_button_catalog()
    check_command_funnel()
    check_state_api_and_sensors()
    check_mqtt_and_ha_surface()
    check_ui_settings()

    if failures:
        print("touchscreen firmware invariant check failed:", file=sys.stderr)
        for failure in failures:
            print(f" - {failure}", file=sys.stderr)
        return 1

    print("touchscreen firmware invariant check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
