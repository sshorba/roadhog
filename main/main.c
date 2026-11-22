#include "connection_status.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "modem.h"
#include "portal.h"
#include "sms_control.h"
#include "status_led.h"

static const char *TAG = "roadhog";

static void log_notification(const char *line, void *context)
{
    if (!sms_control_notification(context, line)) {
        ESP_LOGI(TAG, "Unsolicited: %s", line);
    }
}

static bool probe(modem_t *modem, const char *command, const char *prefix)
{
    modem_result_t result;
    char response[MODEM_LINE_CAPACITY];
    esp_err_t error = modem_command(modem, command, prefix, 2000,
                                    response, sizeof(response), &result);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s", command, esp_err_to_name(error));
    }
    if (response[0]) {
        // The component appends a newline to each solicited line.
        ESP_LOGI(TAG, "%s payload: %s", command, response);
    }
    if (error == ESP_OK && result.final == MODEM_FINAL_OK && prefix && !response[0]) {
        ESP_LOGW(TAG, "%s: expected payload %s", command, prefix);
    }
    if (result.final != MODEM_FINAL_NONE) {
        ESP_LOGI(TAG, "%s final: %s", command, result.final_line);
    }
    return result.final != MODEM_FINAL_NONE;
}

void app_main(void)
{
    static sms_control_t sms;
    esp_err_t led_error = status_led_start();
    if (led_error != ESP_OK) {
        ESP_LOGW(TAG, "Status LED unavailable: %s", esp_err_to_name(led_error));
    }
    esp_err_t portal_error = portal_init();
    if (portal_error != ESP_OK) {
        ESP_LOGE(TAG, "Setup portal unavailable: %s", esp_err_to_name(portal_error));
    }
    const modem_config_t config = {
        .port = UART_NUM_1,
        .tx_pin = GPIO_NUM_17,
        .rx_pin = GPIO_NUM_18,
        .baud_rate = 115200,
        .on_notification = log_notification,
        .context = &sms,
    };
    modem_t *modem;
    esp_err_t error = modem_open(&config, &modem);
    if (error != ESP_OK) {
        portal_set_status(LTE_UNKNOWN, NULL, GPS_UNAVAILABLE);
        ESP_LOGE(TAG, "Cannot open modem UART: %s", esp_err_to_name(error));
        return;
    }
    ESP_LOGI(TAG, "SIM7600 UART1 TX=17 RX=18 baud=115200");

    // Let the UART drain boot notifications before the first probe.
    if (modem_poll(modem, 1000) != ESP_OK) {
        portal_set_status(LTE_UNKNOWN, NULL, GPS_UNAVAILABLE);
        ESP_LOGE(TAG, "Startup UART fault; check modem power and wiring, then restart both devices");
        modem_close(modem);
        return;
    }
    status_led_set_connections(probe(modem, "AT", NULL), false, false);
    portal_set_status(read_lte_registration(modem), NULL, GPS_UNAVAILABLE);
    sms_control_start(&sms, modem);
    start_gps(modem);

    for (;;) {
        sms_control_service(&sms, modem);
        bool modem_reachable = probe(modem, "AT+CPIN?", "+CPIN:");
        lte_status_t lte = read_lte_registration(modem);
        const char *data = lte_data_context(modem);
        gps_fix_t fix;
        gps_status_t gps = read_gps(modem, &fix);
        portal_set_status(lte, data, gps);
        ESP_LOGI(TAG, "LTE: %s, data context: %s, GPS: %s",
                 lte_label(lte), data, gps_label(gps));
        modem_reachable |= probe(modem, "AT+CSQ", "+CSQ:");
        status_led_set_connections(modem_reachable || lte != LTE_UNKNOWN || gps != GPS_UNAVAILABLE,
                                   lte == LTE_HOME || lte == LTE_ROAMING, gps == GPS_FIX);
        if (modem_poll(modem, 5000) != ESP_OK) {
            status_led_set_connections(false, false, false);
            portal_set_status(LTE_UNKNOWN, NULL, GPS_UNAVAILABLE);
            ESP_LOGE(TAG, "UART fault; restart both the modem and ESP32 to resynchronize");
            modem_close(modem);
            return;
        }
    }
}
