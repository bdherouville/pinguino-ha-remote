#!/usr/bin/env python3
"""Source-level contract tests for the touchscreen firmware flavor.

These tests cover firmware behavior that can be validated without attached
hardware. ESP-IDF builds still validate compilation, and HIL/manual tests remain
required for the display, touch, nRF52840, BME680 and Home Assistant runtime path.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware" / "esp32_bridge"
MAIN = FW / "main"

EXPECTED_BUTTONS = [
    ("BRIDGE_BUTTON_POWER", "power", "Power"),
    ("BRIDGE_BUTTON_UP", "up", "Temp Up"),
    ("BRIDGE_BUTTON_DOWN", "down", "Temp Down"),
    ("BRIDGE_BUTTON_MODE", "mode", "Mode"),
    ("BRIDGE_BUTTON_FAN", "fan", "Fan"),
    ("BRIDGE_BUTTON_SILENT", "silent", "Silent"),
    ("BRIDGE_BUTTON_ECO", "eco", "Eco"),
    ("BRIDGE_BUTTON_TIMER", "timer", "Timer"),
    ("BRIDGE_BUTTON_FLAP", "flap", "Flap"),
]


def read(path: Path | str) -> str:
    p = ROOT / path if isinstance(path, str) else path
    return p.read_text(encoding="utf-8")


def strip_c_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//.*", "", text)


class FlavorReleaseContracts(unittest.TestCase):
    def test_flavor_targets_and_names_are_distinct(self) -> None:
        common = read(FW / "sdkconfig.defaults")
        headless = read(FW / "sdkconfig.defaults.headless")
        touchscreen = read(FW / "sdkconfig.defaults.touchscreen")
        kconfig = read(MAIN / "Kconfig.projbuild")

        self.assertNotIn("CONFIG_IDF_TARGET=", common)
        self.assertIn('CONFIG_IDF_TARGET="esp32s3"', headless)
        self.assertIn('CONFIG_IDF_TARGET="esp32"', touchscreen)
        self.assertIn("CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN=y", touchscreen)
        for font in ("14", "16", "20", "24"):
            self.assertIn(f"CONFIG_LV_FONT_MONTSERRAT_{font}=y", touchscreen)
        self.assertIn('default "pinguino-ha-remote-headless"', kconfig)
        self.assertIn('default "pinguino-ha-remote-touch"', kconfig)

    def test_ci_and_release_build_both_flavors(self) -> None:
        ci = read(ROOT / ".github" / "workflows" / "ci.yml")
        release = read(ROOT / ".github" / "workflows" / "release.yml")
        flash_manifest = read(ROOT / "docs" / "flash" / "manifest.json")
        flash_index = read(ROOT / "docs" / "flash" / "index.html")
        top_readme = read(ROOT / "README.md")

        for workflow in (ci, release):
            self.assertIn("flavor: headless", workflow)
            self.assertIn("target: esp32s3", workflow)
            self.assertIn("flavor: touchscreen", workflow)
            self.assertIn("target: esp32", workflow)

        self.assertIn("ganymede-bridge-headless-esp32s3.bin", release)
        self.assertIn("ganymede-bridge-touchscreen-esp32.bin", release)
        self.assertIn("ganymede-bridge-esp32s3.bin", release)
        self.assertIn("bootloader_offset: '0x0'", release)
        self.assertIn("bootloader_offset: '0x1000'", release)

        self.assertIn('"chipFamily": "ESP32-S3"', flash_manifest)
        self.assertIn('"chipFamily": "ESP32"', flash_manifest)
        self.assertIn("ganymede-bridge-headless-esp32s3.bin", flash_manifest)
        self.assertIn("ganymede-bridge-touchscreen-esp32.bin", flash_manifest)
        self.assertIn("plain ESP32 touchscreen", flash_index)
        self.assertIn("ganymede-bridge-touchscreen-esp32.bin", top_readme)
        self.assertIn("touchscreen build targets a plain ESP32", top_readme)

    def test_readme_documents_plain_esp32_touchscreen_flavor(self) -> None:
        readme = read(FW / "README.md")

        self.assertIn("ganymede-bridge-headless-esp32s3.bin", readme)
        self.assertIn("ganymede-bridge-touchscreen-esp32.bin", readme)
        self.assertIn("The touchscreen flavor targets a plain ESP32, not ESP32-S3.", readme)
        self.assertIn("GPIO17 (TX)", readme)
        self.assertIn("GPIO16 (RX)", readme)
        self.assertIn("GPIO33 (SDA)", readme)
        self.assertIn("GPIO32 (SCL)", readme)
        self.assertIn("disabled (`nrf_hb=-1`)", readme)
        self.assertIn("BME680 uses I2C address auto-detection", readme)
        self.assertIn("JC2432W328", readme)
        self.assertIn("ST7789", readme)
        self.assertIn("CST820", readme)


class CommandContracts(unittest.TestCase):
    def test_button_catalog_is_single_source_of_truth(self) -> None:
        header = read(MAIN / "bridge_buttons.h")
        impl = read(MAIN / "bridge_buttons.c")

        macros = re.findall(r"#define\s+(BRIDGE_BUTTON_[A-Z_]+)\s+\"([a-z_]+)\"", header)
        self.assertEqual([(m, v) for m, v, _ in EXPECTED_BUTTONS], macros)

        table = re.findall(r"\{(BRIDGE_BUTTON_[A-Z_]+),\s*\"([^\"]+)\"\}", impl)
        self.assertEqual([(m, label) for m, _, label in EXPECTED_BUTTONS], table)

    def test_button_commands_are_funnelled_through_queue(self) -> None:
        allowed_uart_press = {"uart_link.c", "uart_link.h", "command_queue.c"}
        allowed_uart_write = {"uart_link.c"}
        for path in MAIN.glob("*.c"):
            text = strip_c_comments(read(path))
            with self.subTest(file=path.name):
                if path.name not in allowed_uart_press:
                    self.assertNotIn("uart_link_press(", text)
                if path.name not in allowed_uart_write:
                    self.assertNotIn("uart_write_bytes(", text)

        self.assertIn("bridge_press_button(btn)", read(MAIN / "web.c"))
        self.assertIn("bridge_press_button(btn)", read(MAIN / "mqtt_ha.c"))
        self.assertIn("bridge_press_button(button_name)", read(MAIN / "ui_lvgl.c"))
        self.assertIn("bridge_press_button_wait(b, &sent)", read(MAIN / "ac_cmd.c"))

    def test_uart_wire_protocol_is_preserved(self) -> None:
        uart = read(MAIN / "uart_link.c")
        self.assertIn('"press %s\\n"', uart)
        self.assertIn('"env %.2f %.2f %.2f\\n"', uart)
        self.assertIn('"unpair\\n"', uart)
        self.assertIn('"pair\\n"', uart)


class NrfStateContracts(unittest.TestCase):
    def test_nrf_status_tokens_normalize_to_expected_states(self) -> None:
        uart = read(MAIN / "uart_link.c")
        for token, state in {
            "boot": "NRF_BOOT",
            "advertising": "NRF_ADVERTISING",
            "adv": "NRF_ADVERTISING",
            "pairing": "NRF_ADVERTISING",
            "connected": "NRF_CONNECTED",
            "conn": "NRF_CONNECTED",
            "bonded": "NRF_BONDED",
            "ready": "NRF_READY",
            "error": "NRF_ERROR",
            "err": "NRF_ERROR",
        }.items():
            self.assertRegex(uart, rf'\{{"{token}",\s*{state}\}}')

        for state, text in {
            "NRF_OFFLINE": "offline",
            "NRF_BOOT": "booting",
            "NRF_ADVERTISING": "pairing",
            "NRF_CONNECTED": "busy",
            "NRF_BONDED": "bonded",
            "NRF_READY": "ready",
            "NRF_ERROR": "error",
        }.items():
            self.assertRegex(uart, rf"\[{state}\]\s*=\s*\"{text}\"")

    def test_nrf_timeout_and_ui_enablement_contract(self) -> None:
        uart = read(MAIN / "uart_link.c")
        ui = read(MAIN / "ui_lvgl.c")

        self.assertIn("#define HB_TIMEOUT_MS 3000", uart)
        self.assertIn("alive ? s_reported : NRF_OFFLINE", uart)
        self.assertIn("static int8_t s_hb_gpio = PIN_DEF_NRF_HB", uart)
        self.assertIn("s_hb_gpio >= 0 ? gpio_get_level(s_hb_gpio) : -1", uart)
        self.assertIn("heartbeat disabled", uart)
        handle_line = uart.split("static void handle_line", 1)[1].split("static void reflect", 1)[0]
        self.assertLess(handle_line.index('strncmp(line, "status ", 7)'),
                        handle_line.index("s_last_rx_us = esp_timer_get_time();"))
        self.assertIn("gpio_set_pull_mode((gpio_num_t)pn->nrf_rx, GPIO_PULLUP_ONLY)", uart)
        self.assertIn("return esp_timer_get_time() < s_mute_until_us ||\n           s_effective == NRF_READY;", uart)
        self.assertIn("uart_link_mute_secs() > 0 || st == NRF_READY", ui)


class SensorStateContracts(unittest.TestCase):
    def test_boot_logs_board_pin_map_and_sensor_flavor(self) -> None:
        pins = read(MAIN / "pins.c")
        board = read(MAIN / "board_config.h")
        web = read(MAIN / "web.c")
        main = read(MAIN / "main.c")

        self.assertIn("#define NRF_HEARTBEAT_GPIO -1", board)
        self.assertIn('"board %s pins: i2c sda=%d scl=%d | nrf tx=%d rx=%d hb=%d | ld tx=%d rx=%d"', pins)
        self.assertIn("BOARD_NAME", pins)
        self.assertIn("nvs_get_i8", pins)
        self.assertIn("nvs_get_u8", pins)
        self.assertIn("nvs_set_i8", pins)
        self.assertIn("log_pin_map();", pins)
        self.assertIn('form_pin(body, "nrf_hb",  &p.nrf_hb,  true)', web)
        self.assertIn("headless: BME280, touchscreen: BME680", main)

    def test_jc2432w328_display_touch_are_configured(self) -> None:
        board_h = read(MAIN / "board_config.h")
        board_c = read(MAIN / "board_config.c")

        for token in (
            '#define BOARD_MODEL "JC2432W328"',
            '#define DISPLAY_DRIVER_NAME "ST7789"',
            "#define DISPLAY_H_RES 320",
            "#define DISPLAY_V_RES 240",
            "#define DISPLAY_SPI_MISO_GPIO 12",
            "#define DISPLAY_SPI_MOSI_GPIO 13",
            "#define DISPLAY_SPI_SCLK_GPIO 14",
            "#define DISPLAY_SPI_CS_GPIO 15",
            "#define DISPLAY_DC_GPIO 2",
            "#define DISPLAY_BACKLIGHT_GPIO 27",
            '#define TOUCH_DRIVER_NAME "CST820"',
            "#define TOUCH_I2C_ADDR_CST820 0x15",
            "#define TOUCH_RST_GPIO 25",
            "#define TOUCH_INT_GPIO 21",
            "#define BME_I2C_SDA_GPIO 33",
            "#define BME_I2C_SCL_GPIO 32",
            "#define LD2410_UART_TX_GPIO 19",
            "#define LD2410_UART_RX_GPIO 18",
        ):
            self.assertIn(token, board_h)

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
            self.assertIn(token, board_c)

        self.assertNotIn('DISPLAY_DRIVER_NAME "unconfigured"', board_h)
        self.assertNotIn('TOUCH_DRIVER_NAME "unconfigured"', board_h)

    def test_bme680_absence_stale_and_air_quality_are_handled(self) -> None:
        bme = read(MAIN / "bme280.c")
        state = read(MAIN / "bridge_state.c")

        self.assertIn("BME680_I2C_ADDR_0", bme)
        self.assertIn("BME680_I2C_ADDR_1", bme)
        self.assertIn("bridge_state_update_sensor(false, \"BME680\"", bme)
        self.assertIn("#define BME_STALE_MS 120000ULL", bme)
        self.assertIn("#define SENSOR_STALE_MS 120000ULL", state)

        self.assertIn('return "Unknown"', bme)
        self.assertIn('return "Good"', bme)
        self.assertIn('return "Average"', bme)
        self.assertIn('return "Poor"', bme)
        self.assertIn("ratio >= 0.8f", bme)
        self.assertIn("ratio >= 0.5f", bme)


class ApiMqttHaContracts(unittest.TestCase):
    def test_status_api_extends_schema_without_exposing_secrets(self) -> None:
        web = read(MAIN / "web.c")
        status = web.split("static esp_err_t h_status", 1)[1].split("static esp_err_t h_scan", 1)[0]

        for key in (
            "state",
            "ssid",
            "ip",
            "mqtt",
            "firmware",
            "wifi",
            "mqtt_detail",
            "nrf_detail",
            "sensor",
            "display",
            "command",
            "runtime",
            "gas_resistance_ohm",
            "air_quality",
            "screen_timeout_s",
        ):
            self.assertIn(f'\\"{key}\\"', status)

        self.assertNotIn('"pass"', status)
        self.assertNotIn("s_pass", status)
        self.assertIn("static void json_escape_str", web)
        self.assertIn("const size_t buflen = 4096", status)
        self.assertIn("json_escape_str(st.wifi_ssid", status)
        self.assertIn("json_escape_str(st.mqtt_host", status)
        self.assertIn("json_escape_str(st.nrf_last_message", status)

    def test_scan_api_escapes_ssids(self) -> None:
        web = read(MAIN / "web.c")
        scan = web.split("static esp_err_t h_scan", 1)[1].split("static esp_err_t h_connect", 1)[0]

        self.assertIn("json_escape_str(aps[i].ssid", scan)
        self.assertIn('{\\"ssid\\":\\"%s\\",\\"rssi\\":%d,\\"auth\\":%d}', scan)

    def test_press_and_ui_api_return_contracts(self) -> None:
        web = read(MAIN / "web.c")
        press = web.split("static esp_err_t h_press", 1)[1].split("static esp_err_t h_ui", 1)[0]
        ui = web.split("static esp_err_t h_ui", 1)[1].split("static esp_err_t h_pairing", 1)[0]

        self.assertIn('{\\"ok\\":true,\\"button\\":\\"%s\\",\\"queued\\":true}', press)
        self.assertIn('{\\"ok\\":false,\\"error\\":\\"nrf_unavailable\\"}', press)
        self.assertIn('{\\"ok\\":false,\\"error\\":\\"queue_full\\"}', press)
        self.assertIn('{\\"ok\\":false,\\"error\\":\\"bad_button\\"}', press)

        for key in ("screen", "brightness", "timeout", "theme", "wake"):
            self.assertIn(f'json_value_get(body, "{key}"', ui)
        self.assertIn('json_string_get(body, "button"', ui)

    def test_mqtt_and_home_assistant_surface_is_present(self) -> None:
        mqtt = read(MAIN / "mqtt_ha.c")

        for token in (
            '#define STATE_AVTY_TOPIC AVTY_TOPIC',
            '#define CMD_PREFIX "ganymede/cmd/"',
            '#define NRF_STATE_TOPIC "ganymede/state/nrf_status"',
            '#define LAST_BUTTON_TOPIC "ganymede/state/last_button"',
            '#define SENSOR_STATE_BASE "ganymede/state/sensor/"',
            '#define DISPLAY_BRIGHTNESS_TOPIC "ganymede/state/display/brightness"',
            '#define WIFI_RSSI_TOPIC "ganymede/state/wifi/rssi"',
            "homeassistant/button/ganymede_%s/config",
            "homeassistant/number/ganymede_display_brightness/config",
            "homeassistant/select/ganymede_display_theme/config",
            "esp_mqtt_client_subscribe(s_client, CMD_PREFIX \"+\", 1)",
        ):
            self.assertIn(token, mqtt)
        self.assertLess(
            mqtt.index('#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN\n    snprintf(topic, sizeof(topic), "homeassistant/number/ganymede_display_brightness/config")'),
            mqtt.index("#endif\n    // LD2410 presence"),
        )
        self.assertLess(
            mqtt.index("#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN\n        esp_mqtt_client_subscribe(s_client, DISPLAY_BRIGHTNESS_CMD_TOPIC, 1);"),
            mqtt.index("#endif\n        { ac_state_t snap;"),
        )
        self.assertLess(
            mqtt.index("#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN\n        } else if (!strcmp(topic, DISPLAY_BRIGHTNESS_CMD_TOPIC))"),
            mqtt.index("#endif\n        }\n        break;"),
        )


class UiContracts(unittest.TestCase):
    def test_lvgl_startup_and_settings_contract(self) -> None:
        ui = read(MAIN / "ui_lvgl.c")
        start = ui.split("esp_err_t ui_lvgl_start(void)", 1)[1].split("bool ui_lvgl_display_available", 1)[0]

        self.assertLess(start.index("lv_init();"), start.index("esp_err_t display = board_display_init();"))
        self.assertLess(start.index("esp_err_t display = board_display_init();"), start.index("init_lvgl_screens();"))

        for token in (
            "#define UI_TIMEOUT_DEFAULT_S 60",
            "static uint8_t s_brightness_percent = 100",
            'static char s_theme[8] = "dark"',
            "lv_tileview_create",
            "lv_tileview_add_tile",
            "lv_tileview_set_tile_by_index",
            "lv_tileview_get_tile_active",
            "lv_obj_set_scrollbar_mode",
            "LV_SCROLLBAR_MODE_OFF",
            "screen_index_for_name",
            "activity_event",
            "LV_EVENT_PRESSED",
            "Queued: %s",
            'nvs_set_u8(h, "bright"',
            'nvs_set_u16(h, "timeout"',
            'nvs_set_str(h, "theme"',
            "wifi_mgr_reset_provisioning",
        ):
            self.assertIn(token, ui)


if __name__ == "__main__":
    unittest.main(verbosity=2)
