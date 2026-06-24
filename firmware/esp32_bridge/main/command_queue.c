#include "command_queue.h"
#include "bridge_state.h"
#include "uart_link.h"
#include "mqtt_ha.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

typedef struct {
    char button[12];
    QueueHandle_t reply;
} bridge_command_t;

static const char *TAG = "bridge_cmd";
static QueueHandle_t s_queue;
static portMUX_TYPE s_last_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_last_button[12];
static uint64_t s_last_button_ms;
static bool s_last_sent;

static void update_last_button(const char *button, bool sent)
{
    portENTER_CRITICAL(&s_last_mux);
    strlcpy(s_last_button, button ? button : "", sizeof(s_last_button));
    s_last_button_ms = esp_timer_get_time() / 1000ULL;
    s_last_sent = sent;
    portEXIT_CRITICAL(&s_last_mux);
    bridge_state_update_last_command(button, s_last_button_ms, sent, sent ? "" : "uart_link_not_ready");
}

static void command_worker(void *arg)
{
    bridge_command_t cmd;
    for (;;) {
        if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        bool sent = uart_link_press(cmd.button);
        update_last_button(cmd.button, sent);
        if (!sent) {
            ESP_LOGW(TAG, "button '%s' was dropped by UART link", cmd.button);
        } else {
            mqtt_ha_publish_last_button(cmd.button);
        }
        if (cmd.reply) {
            xQueueSend(cmd.reply, &sent, 0);
        }
    }
}

bool bridge_last_button(char *out, size_t outlen, uint64_t *timestamp_ms, bool *sent)
{
    portENTER_CRITICAL(&s_last_mux);
    bool has_value = s_last_button[0] != 0;
    if (out && outlen) strlcpy(out, s_last_button, outlen);
    if (timestamp_ms) *timestamp_ms = s_last_button_ms;
    if (sent) *sent = s_last_sent;
    portEXIT_CRITICAL(&s_last_mux);
    return has_value;
}

void bridge_command_queue_init(void)
{
    if (s_queue) return;
    s_queue = xQueueCreate(8, sizeof(bridge_command_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "command queue allocation failed");
        return;
    }
    xTaskCreate(command_worker, "bridge_cmd", 3072, NULL, 5, NULL);
}

esp_err_t bridge_press_button(const char *button_name)
{
    return bridge_press_button_wait(button_name, NULL);
}

esp_err_t bridge_press_button_wait(const char *button_name, bool *sent)
{
    if (!button_name || !button_name[0] || !uart_link_valid_btn(button_name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!uart_link_will_model()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    bridge_command_t cmd = {0};
    strlcpy(cmd.button, button_name, sizeof(cmd.button));
    QueueHandle_t reply = NULL;
    if (sent) {
        reply = xQueueCreate(1, sizeof(bool));
        if (!reply) return ESP_ERR_NO_MEM;
        cmd.reply = reply;
    }

    if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        if (reply) vQueueDelete(reply);
        ESP_LOGW(TAG, "command queue full, dropping '%s'", button_name);
        return ESP_ERR_TIMEOUT;
    }
    if (reply) {
        bool result = false;
        xQueueReceive(reply, &result, portMAX_DELAY);
        vQueueDelete(reply);
        if (sent) *sent = result;
        if (!result) return ESP_FAIL;
    }
    return ESP_OK;
}
