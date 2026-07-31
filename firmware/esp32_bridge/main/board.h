#pragma once
#include <stdbool.h>
#include "sdkconfig.h"     // CONFIG_IDF_TARGET_* come from here

// Per-chip board definitions: status LED and default peripheral pins.
//
// The BLE emulator (ble_emu.c) is chip-agnostic — any ESP32 with a BLE radio can be the
// Ganymede remote. Everything that is NOT portable lives here, because the pin numbers and the
// status LED differ per chip and a wrong one is either a build error or a silent failure:
//
//   * ESP32-S3 SuperMini: WS2812 on GPIO48. The C3 has no GPIO48 at all (it stops at 21), so
//     the old hard-coded 48 would fail led_strip init on any other target.
//   * ESP32-C3: GPIO11 is VDD_SPI and 12-17 are the SPI flash — touching those kills the
//     board. GPIO18/19 are the native USB D-/D+ lines, i.e. the port you flash and monitor
//     over, so the old GPIO18 default for the LD2410 would have cost you the console.
//
// Chips without a BLE radio are rejected outright rather than failing confusingly later.
#if !defined(CONFIG_IDF_TARGET_ESP32S3) && !defined(CONFIG_IDF_TARGET_ESP32C3) && \
    !defined(CONFIG_IDF_TARGET_ESP32C6) && !defined(CONFIG_IDF_TARGET_ESP32)
#error "Unsupported target. This firmware needs a chip with BLE + Wi-Fi: ESP32, ESP32-S3, \
ESP32-C3 or ESP32-C6. The ESP32-S2 has no Bluetooth, the ESP32-H2 has no Wi-Fi, and the \
ESP32-P4 has no radio at all. Run tools/identify-boards.sh to check what you have."
#endif

// ---- status LED ------------------------------------------------------------------------
// BOARD_LED_WS2812 : addressable RGB, driven over RMT (full colour states).
// BOARD_LED_PLAIN  : a single-colour LED on a plain GPIO; colours collapse to on/off, so the
//                    states are still distinguishable by their blink pattern.
// BOARD_LED_NONE   : no LED; led_status becomes a no-op (the web UI and MQTT still report).
#define BOARD_LED_NONE    0
#define BOARD_LED_WS2812  1
#define BOARD_LED_PLAIN   2

#if defined(CONFIG_IDF_TARGET_ESP32S3)
  // ESP32-S3 SuperMini. Some board revisions fit a plain LED here instead of the WS2812; it
  // then drives nothing harmful, the colours just don't render.
  #define BOARD_NAME        "esp32s3"
  #define BOARD_LED_KIND    BOARD_LED_WS2812
  #define BOARD_LED_GPIO    48
  #define BOARD_LED_ACTIVE_HIGH 1
  #define BOARD_PIN_I2C_SDA 2
  #define BOARD_PIN_I2C_SCL 1
  #define BOARD_PIN_LD_TX   17
  #define BOARD_PIN_LD_RX   18

#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  // ESP32-C3. GPIO8 is where both the DevKitM-1's WS2812 and most SuperMini-style clones put
  // their LED — but it is also a strapping pin (it must read high at reset alongside GPIO9).
  // Driving it after boot is fine and is exactly what the Espressif dev kits do.
  //
  // Pins chosen to dodge every trap: 0/1 can be the 32 kHz crystal, 2/8/9 are strapping,
  // 11-17 are VDD_SPI + SPI flash, 18/19 are USB, 20/21 are UART0. That leaves 3-7 and 10.
  #define BOARD_NAME        "esp32c3"
  #define BOARD_LED_KIND    BOARD_LED_WS2812
  #define BOARD_LED_GPIO    8
  #define BOARD_LED_ACTIVE_HIGH 1
  #define BOARD_PIN_I2C_SDA 5
  #define BOARD_PIN_I2C_SCL 6
  #define BOARD_PIN_LD_TX   7
  #define BOARD_PIN_LD_RX   10

#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  // ESP32-C6 (e.g. DevKitC-1: WS2812 on GPIO8). On the Waveshare Touch-LCD-1.83 the LCD owns
  // GPIO1-6 and I2C is already on 7/8 shared with the codec/touch/PMU — put the BME280 on that
  // same bus (it is a bus; the 0x76/0x77 address does not clash) and change these accordingly.
  #define BOARD_NAME        "esp32c6"
  #define BOARD_LED_KIND    BOARD_LED_WS2812
  #define BOARD_LED_GPIO    8
  #define BOARD_LED_ACTIVE_HIGH 1
  #define BOARD_PIN_I2C_SDA 7
  #define BOARD_PIN_I2C_SCL 6
  #define BOARD_PIN_LD_TX   16
  #define BOARD_PIN_LD_RX   17

#else /* CONFIG_IDF_TARGET_ESP32 — original dual-core ESP32 */
  // Classic ESP32 (WROOM-32). No native USB, so the console is UART0 on GPIO1/3 — leave those
  // alone. GPIO6-11 are the SPI flash. Most dev boards have a plain LED on GPIO2.
  #define BOARD_NAME        "esp32"
  #define BOARD_LED_KIND    BOARD_LED_PLAIN
  #define BOARD_LED_GPIO    2
  #define BOARD_LED_ACTIVE_HIGH 1
  #define BOARD_PIN_I2C_SDA 21
  #define BOARD_PIN_I2C_SCL 22
  #define BOARD_PIN_LD_TX   17
  #define BOARD_PIN_LD_RX   16
#endif

// ---- usable GPIO range -----------------------------------------------------------------
// True if g is a pin the user may assign to a peripheral on this chip. Deliberately strict:
// rejecting a usable pin costs a config option, allowing a flash pin bricks the board.
static inline bool board_gpio_assignable(int g)
{
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    if (g < 0 || g > 48)    return false;
    if (g >= 22 && g <= 25) return false;   // not bonded out
    if (g >= 26 && g <= 32) return false;   // SPI flash / PSRAM
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
    if (g < 0 || g > 21)    return false;
    if (g >= 11 && g <= 17) return false;   // VDD_SPI (11) + SPI flash (12-17)
    if (g == 18 || g == 19) return false;   // native USB D-/D+ : the flash & console port
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
    if (g < 0 || g > 30)    return false;
    if (g >= 24 && g <= 30) return false;   // SPI flash
    if (g == 12 || g == 13) return false;   // native USB D-/D+
#else /* ESP32 */
    if (g < 0 || g > 39)    return false;
    if (g >= 6 && g <= 11)  return false;   // SPI flash
    if (g == 1 || g == 3)   return false;   // UART0 console (no native USB on this chip)
    if (g == 20 || g == 24) return false;   // not bonded out
    if (g >= 34 && g <= 39) return false;   // input-only, no output driver
#endif
    if (g == BOARD_LED_GPIO) return false;  // owned by the status LED
    return true;
}
