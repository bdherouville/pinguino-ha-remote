#pragma once

#include <stdint.h>
#include "driver/i2c_types.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "hal/spi_types.h"

#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
#define BOARD_NAME "esp32_touchscreen"
#else
#define BOARD_NAME "esp32s3_headless"
#endif

#define NRF_UART_PORT UART_NUM_1
#define NRF_UART_BAUDRATE 115200
#ifdef CONFIG_IDF_TARGET_ESP32
#define NRF_UART_TX_GPIO 17
#define NRF_UART_RX_GPIO 16
#define NRF_HEARTBEAT_GPIO -1

#define BME_I2C_PORT I2C_NUM_0
#define BME_I2C_SDA_GPIO 33
#define BME_I2C_SCL_GPIO 32
#define BME_I2C_FREQ_HZ 400000

#define LD2410_UART_TX_GPIO 19
#define LD2410_UART_RX_GPIO 18

#define STATUS_LED_GPIO -1
#else
#define NRF_UART_TX_GPIO 4
#define NRF_UART_RX_GPIO 5
#define NRF_HEARTBEAT_GPIO 6

#define BME_I2C_PORT I2C_NUM_0
#define BME_I2C_SDA_GPIO 2
#define BME_I2C_SCL_GPIO 1
#define BME_I2C_FREQ_HZ 400000

#define LD2410_UART_TX_GPIO 17
#define LD2410_UART_RX_GPIO 18

#define STATUS_LED_GPIO 48
#endif

#define BME680_I2C_PORT BME_I2C_PORT
#define BME680_I2C_SDA_GPIO BME_I2C_SDA_GPIO
#define BME680_I2C_SCL_GPIO BME_I2C_SCL_GPIO
#define BME680_I2C_FREQ_HZ BME_I2C_FREQ_HZ

#ifdef CONFIG_PINGUINO_FIRMWARE_TOUCHSCREEN
#define BOARD_MODEL "JC2432W328"

#define DISPLAY_DRIVER_NAME "ST7789"
#define DISPLAY_H_RES 320
#define DISPLAY_V_RES 240
#define DISPLAY_SPI_HOST SPI2_HOST
#define DISPLAY_SPI_MISO_GPIO 12
#define DISPLAY_SPI_MOSI_GPIO 13
#define DISPLAY_SPI_SCLK_GPIO 14
#define DISPLAY_SPI_CS_GPIO 15
#define DISPLAY_DC_GPIO 2
#define DISPLAY_RST_GPIO -1
#define DISPLAY_BACKLIGHT_GPIO 27

#define TOUCH_DRIVER_NAME "CST820"
#define TOUCH_I2C_PORT BME_I2C_PORT
#define TOUCH_I2C_SDA_GPIO BME_I2C_SDA_GPIO
#define TOUCH_I2C_SCL_GPIO BME_I2C_SCL_GPIO
#define TOUCH_I2C_FREQ_HZ 400000
#define TOUCH_I2C_ADDR_CST820 0x15
#define TOUCH_RST_GPIO 25
#define TOUCH_INT_GPIO 21
#else
#define BOARD_MODEL "headless"
#define DISPLAY_BACKLIGHT_GPIO -1
#define DISPLAY_DRIVER_NAME "none"
#define TOUCH_DRIVER_NAME "none"
#endif

esp_err_t board_display_init(void);
esp_err_t board_touch_init(void);
esp_err_t board_backlight_set(uint8_t percent);
typedef void (*board_touch_activity_cb_t)(void);
void board_touch_set_activity_cb(board_touch_activity_cb_t cb);
