#include "sdkconfig.h"
#include "connection_status.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lte_diagnostic.h"
#include "modem.h"
#include "ota_boot_health.h"
#include "portal.h"
#include "provisioning.h"
#include "sim_number.h"
#include <string.h>
#include "sms_control.h"
#include "status_led.h"

static const char *TAG = "roadhog";

static void log_notification(const char *line, void *context)
{
    if (!sms_control_notification(context, line) &&
        provisioning_state() == PROVISION_CONFIGURED && strncmp(line, "+CNUM:", 6)) {
        ESP_LOGI(TAG, "Unsolicited: %s", line);
    }
}

static modem_final_t probe(modem_t *modem, const char *command, const char *prefix)
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
    return result.final;
}

static void cache_sim_number(modem_t *modem)
{
    char response[2 * MODEM_LINE_CAPACITY];
    char number[SIM_NUMBER_CAPACITY] = {0};
    modem_result_t result;
    if (modem_command(modem, "AT+CNUM", "+CNUM:", 2000,
                      response, sizeof(response), &result) == ESP_OK &&
        result.final == MODEM_FINAL_OK) {
        sim_number_parse(response, number);
    }
    portal_set_sim_number(number);
}

void app_main(void)
{
    static sms_control_t sms;
    esp_err_t led_error = status_led_start();
    if (led_error != ESP_OK) {
        ESP_LOGW(TAG, "Status LED unavailable: %s", esp_err_to_name(led_error));
    }
    provisioning_init();
    esp_err_t network_error = esp_netif_init();
    if (network_error == ESP_OK) {
        network_error = esp_event_loop_create_default();
    }
    esp_err_t portal_error = network_error == ESP_OK ? portal_init() : network_error;
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
        ota_boot_health_finish(false);
        return;
    }
    ESP_LOGI(TAG, "SIM7600 UART1 TX=17 RX=18 baud=115200");

    // Let the UART drain boot notifications before the first probe.
    if (modem_poll(modem, 1000) != ESP_OK) {
        portal_set_status(LTE_UNKNOWN, NULL, GPS_UNAVAILABLE);
        ESP_LOGE(TAG, "Startup UART fault; check modem power and wiring, then restart both devices");
        modem_close(modem);
        ota_boot_health_finish(false);
        return;
    }
    modem_final_t at = probe(modem, "AT", NULL);
    status_led_set_connections(at != MODEM_FINAL_NONE, false, false);
    portal_set_status(read_lte_registration(modem), NULL, GPS_UNAVAILABLE);
    sms_control_start(&sms, modem);
    bool gps_started = start_gps(modem);

#ifdef CONFIG_ROADHOG_SMS_CONTROL
    bool sms_ready = provisioning_state() == PROVISION_FACTORY || sms.configured;
#else
    bool sms_ready = true;
#endif
    ota_boot_health_finish(portal_error == ESP_OK && at == MODEM_FINAL_OK &&
                           sms_ready && gps_started);
    if (at == MODEM_FINAL_OK) {
        cache_sim_number(modem);
    }

#ifdef CONFIG_ROADHOG_LTE_DIAGNOSTIC
    bool diagnostic_pending = network_error == ESP_OK && provisioning_apn() != NULL;
    const int64_t diagnostic_at = esp_timer_get_time() + CONFIG_ROADHOG_LTE_DELAY * 1000000LL;
    if (diagnostic_pending) {
        ESP_LOGI(TAG, "LTE diagnostic scheduled in %d seconds", CONFIG_ROADHOG_LTE_DELAY);
    } else {
        ESP_LOGE(TAG, "LTE diagnostic unavailable: network or provisioning is not ready");
    }
#endif

    for (;;) {
        sms_control_service(&sms, modem);
        bool modem_reachable = probe(modem, "AT+CPIN?", "+CPIN:") != MODEM_FINAL_NONE;
        lte_status_t lte = read_lte_registration(modem);
        if (provisioning_state() == PROVISION_FACTORY && (lte == LTE_HOME || lte == LTE_ROAMING)) {
            sms_owner_verification_service(modem);
        }
        const char *data = lte_data_context(modem);
        gps_fix_t fix;
        gps_status_t gps = read_gps(modem, &fix);
        portal_set_status(lte, data, gps);
        ESP_LOGI(TAG, "LTE: %s, data context: %s, GPS: %s",
                 lte_label(lte), data, gps_label(gps));
        bool csq_answered = probe(modem, "AT+CSQ", "+CSQ:") != MODEM_FINAL_NONE;
        modem_reachable |= csq_answered;
        status_led_set_connections(modem_reachable || lte != LTE_UNKNOWN || gps != GPS_UNAVAILABLE,
                                   lte == LTE_HOME || lte == LTE_ROAMING, gps == GPS_FIX);
#ifdef CONFIG_ROADHOG_LTE_DIAGNOSTIC
        if (diagnostic_pending && esp_timer_get_time() >= diagnostic_at) {
            diagnostic_pending = false;
            // A missing final can mean an older AT command still owns the stream.
            if (!csq_answered) {
                ESP_LOGW(TAG, "LTE diagnostic skipped: AT command stream is not idle");
            } else if (lte == LTE_HOME || lte == LTE_ROAMING) {
                ESP_LOGI(TAG, "LTE registration confirmed: %s", lte_label(lte));
                bool uart_reopened;
                modem = lte_diagnostic_run(modem, &config, &uart_reopened);
                if (!modem) {
                    status_led_set_connections(false, false, false);
                    portal_set_status(LTE_UNKNOWN, NULL, GPS_UNAVAILABLE);
                    ESP_LOGE(TAG, "Restart both devices to restore SMS and GNSS");
                    return;
                }
                if (uart_reopened) {
                    sms_control_resume(&sms, modem);
                    start_gps(modem);
                }
            } else {
                ESP_LOGW(TAG, "LTE diagnostic skipped at deadline: %s", lte_label(lte));
            }
        }
#endif
        if (modem_poll(modem, 5000) != ESP_OK) {
            status_led_set_connections(false, false, false);
            portal_set_status(LTE_UNKNOWN, NULL, GPS_UNAVAILABLE);
            ESP_LOGE(TAG, "UART fault; restart both the modem and ESP32 to resynchronize");
            modem_close(modem);
            return;
        }
    }
}
