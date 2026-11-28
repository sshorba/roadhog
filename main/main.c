#include "sdkconfig.h"
#include "connection_status.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lte_diagnostic.h"
#include "modem.h"
#include "modem_lifecycle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_boot_health.h"
#if defined(ROADHOG_SMS_OTA_DESCRIPTOR) || defined(CONFIG_ROADHOG_OTA_SMS_BENCH)
#include "ota_report.h"
#endif
#include "ota_update.h"
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
    modem_result_t result = {0};
    char response[MODEM_LINE_CAPACITY] = {0};
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

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool sms_ready(const sms_control_t *sms)
{
#ifdef CONFIG_ROADHOG_SMS_CONTROL
    return provisioning_state() == PROVISION_FACTORY || sms->configured;
#else
    (void)sms;
    return true;
#endif
}

static void check_boot_health(modem_lifecycle_t *lifecycle, int64_t now, bool healthy)
{
    bool previously_blocked = lifecycle->health_blocked;
    modem_lifecycle_check_health(lifecycle, now, healthy);
    if (!previously_blocked && lifecycle->health_blocked) {
        ESP_LOGE(TAG, "OTA health unresolved; PPP and OTA are blocked");
    }
}

static bool initialize_modem(const modem_config_t *config, modem_t **modem, sms_control_t *sms)
{
    esp_err_t error = *modem ? ESP_OK : modem_open(config, modem);
    if (error == ESP_OK) {
        error = modem_poll(*modem, 1000);
    }
    if (error == ESP_OK) {
        error = modem_sync_startup(*modem);
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Modem UART or command mode unavailable: %s", esp_err_to_name(error));
        return false;
    }
    modem_watch_restart(*modem);
    if (probe(*modem, "AT", NULL) != MODEM_FINAL_OK) {
        return false;
    }
    sms_control_start(sms, *modem);
    return sms_ready(sms) && start_gps(*modem) && modem_maintenance_ready(*modem);
}

static void modem_disconnected(modem_lifecycle_t *lifecycle, modem_t **modem, sms_control_t *sms)
{
    bool was_online = lifecycle->phase == MODEM_ONLINE;
    modem_close(*modem);
    *modem = NULL;
    sms_control_disconnect(sms);
    status_led_set_connections(false, false, false);
    portal_set_status(LTE_UNKNOWN, NULL, GPS_UNAVAILABLE);
    portal_set_sim_number("");
    int64_t now = now_ms();
    modem_lifecycle_result(lifecycle, false, now);
    if (was_online) {
        // Reattach promptly to catch the modem's next startup indication.
        lifecycle->retry_at = now;
        ESP_LOGW(TAG, "Modem offline; reconnecting UART");
    } else {
        ESP_LOGW(TAG, "Modem offline; retrying in %u seconds", MODEM_RETRY_MS / 1000);
    }
}

static bool wait_for_at_boundary(modem_t *modem)
{
    return modem_maintenance_ready(modem) ||
           (modem_poll(modem, 1000) == ESP_OK && modem_maintenance_ready(modem));
}

static bool sample_modem_status(modem_t *modem, lte_status_t *lte, gps_status_t *gps,
                                const char **data, bool *gps_running)
{
    gps_session_t session = gps_session_state(modem);
    *gps_running = session == GPS_SESSION_RUNNING ||
                   (session == GPS_SESSION_STOPPED && start_gps(modem));
    if (!wait_for_at_boundary(modem)) {
        return false;
    }

    probe(modem, "AT+CPIN?", "+CPIN:");
    *lte = read_lte_registration(modem);
    *data = lte_data_context(modem);
    gps_fix_t fix;
    *gps = *gps_running ? read_gps(modem, &fix) : GPS_UNAVAILABLE;
    if (!wait_for_at_boundary(modem)) {
        return false;
    }
    return probe(modem, "AT+CSQ", "+CSQ:") != MODEM_FINAL_NONE && wait_for_at_boundary(modem);
}

void app_main(void)
{
    static sms_control_t sms;
#ifdef CONFIG_ROADHOG_OTA_SMS_BENCH
    ota_sms_init();
#endif
    modem_lifecycle_t lifecycle = {
        .health_deadline = now_ms() + MODEM_BOOT_HEALTH_MS,
    };
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
    modem_t *modem = NULL;
    portal_set_status(LTE_UNKNOWN, NULL, GPS_UNAVAILABLE);
    ESP_LOGI(TAG, "SIM7600 UART1 TX=17 RX=18 baud=115200");

#if defined(CONFIG_ROADHOG_LTE_DIAGNOSTIC) && !defined(CONFIG_ROADHOG_OTA_UPDATE_BENCH)
    bool diagnostic_pending = network_error == ESP_OK && provisioning_apn() != NULL;
    const int64_t diagnostic_at = esp_timer_get_time() + CONFIG_ROADHOG_LTE_DELAY * 1000000LL;
    if (diagnostic_pending) {
        ESP_LOGI(TAG, "LTE diagnostic scheduled in %d seconds", CONFIG_ROADHOG_LTE_DELAY);
    } else {
        ESP_LOGE(TAG, "LTE diagnostic unavailable: network or provisioning is not ready");
    }
#endif

    for (;;) {
        int64_t now = now_ms();
        if (lifecycle.phase != MODEM_ONLINE || now >= lifecycle.health_deadline) {
            check_boot_health(&lifecycle, now, false);
        }
        if (lifecycle.phase != MODEM_ONLINE) {
#ifdef CONFIG_ROADHOG_LTE_DIAGNOSTIC
            if (lte_session_owns_uart()) {
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
#endif
            if (!modem_lifecycle_begin(&lifecycle, now)) {
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            bool online = initialize_modem(&config, &modem, &sms);
            if (sms.users_store && !portal_bound_sms()) {
                portal_bind_sms(&sms);
            }
            now = now_ms();
            check_boot_health(&lifecycle, now, online && portal_error == ESP_OK);
            if (!online) {
                if (modem_reconnect_pending(modem)) {
                    // Keep listening across backoff so a late final or RDY is
                    // not lost while the UART driver is absent.
                    modem_lifecycle_result(&lifecycle, false, now);
                } else {
                    modem_disconnected(&lifecycle, &modem, &sms);
                }
                continue;
            }
            modem_lifecycle_result(&lifecycle, true, now);
            ESP_LOGI(TAG, "Modem online; SMS and GNSS initialization complete");
            cache_sim_number(modem);
        }

        lte_status_t lte;
        gps_status_t gps;
        const char *data;
        bool gps_running;
        if (!sample_modem_status(modem, &lte, &gps, &data, &gps_running)) {
            modem_disconnected(&lifecycle, &modem, &sms);
            continue;
        }
        check_boot_health(&lifecycle, now_ms(),
                          gps_running && sms_ready(&sms) && portal_error == ESP_OK);
        portal_set_status(lte, data, gps);
        ESP_LOGI(TAG, "LTE: %s, data context: %s, GPS: %s",
                 lte_label(lte), data, gps_label(gps));
        status_led_set_connections(true,
                                   lte == LTE_HOME || lte == LTE_ROAMING, gps == GPS_FIX);

        sms_control_service(&sms, modem);
        if (provisioning_state() == PROVISION_FACTORY && (lte == LTE_HOME || lte == LTE_ROAMING)) {
            sms_owner_verification_service(modem);
        }
        if (!wait_for_at_boundary(modem) || !sms_ready(&sms)) {
            modem_disconnected(&lifecycle, &modem, &sms);
            continue;
        }
#if defined(CONFIG_ROADHOG_LTE_DIAGNOSTIC) && !defined(CONFIG_ROADHOG_OTA_UPDATE_BENCH)
        if (lifecycle.health_finished && diagnostic_pending && esp_timer_get_time() >= diagnostic_at) {
            diagnostic_pending = false;
            if (lte == LTE_HOME || lte == LTE_ROAMING) {
                ESP_LOGI(TAG, "LTE registration confirmed: %s", lte_label(lte));
                bool uart_reopened;
                modem = lte_diagnostic_run(modem, &config, &uart_reopened);
                if (!modem) {
                    modem_disconnected(&lifecycle, &modem, &sms);
                } else if (uart_reopened) {
                    sms_control_resume(&sms, modem);
                    if (!start_gps(modem)) {
                        modem_disconnected(&lifecycle, &modem, &sms);
                    }
                }
                continue;
            }
            ESP_LOGW(TAG, "LTE diagnostic skipped at deadline: %s", lte_label(lte));
        }
#endif
#if defined(ROADHOG_SMS_OTA_DESCRIPTOR) || defined(CONFIG_ROADHOG_OTA_SMS_BENCH)
        if (lifecycle.health_finished && gps != GPS_UNAVAILABLE &&
            (lte == LTE_HOME || lte == LTE_ROAMING)) {
            ota_sms_report_boot(&sms, modem);
            if (!modem_maintenance_ready(modem)) {
                modem_disconnected(&lifecycle, &modem, &sms);
                continue;
            }
        }
#endif
#ifdef CONFIG_ROADHOG_OTA_UPDATE_BENCH
        if (lifecycle.health_finished && network_error == ESP_OK) {
            modem = ota_update_poll(modem, &config, &sms);
            if (!modem) {
                modem_disconnected(&lifecycle, &modem, &sms);
                continue;
            }
        }
#endif
        if (modem_poll(modem, 5000) != ESP_OK) {
            modem_disconnected(&lifecycle, &modem, &sms);
        }
    }
}
