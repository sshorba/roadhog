#include "sdkconfig.h"
#include "lte_diagnostic.h"

#ifdef CONFIG_ROADHOG_LTE_DIAGNOSTIC

#include "cellular_net.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "lte_diagnostic";

typedef struct {
    cellular_net_config_t config;
    SemaphoreHandle_t done;
    esp_err_t result;
    bool at_ready;
} diagnostic_t;

static void run_diagnostic(void *context)
{
    diagnostic_t *diagnostic = context;
    diagnostic->result = cellular_net_test(&diagnostic->config, &diagnostic->at_ready);
    xSemaphoreGive(diagnostic->done);
    vTaskDelete(NULL);
}

modem_t *lte_diagnostic_run(modem_t *modem, const modem_config_t *config, bool *uart_reopened)
{
    *uart_reopened = false;
    if (!cellular_apn_valid(CONFIG_ROADHOG_LTE_APN)) {
        ESP_LOGE(TAG, "Set a valid carrier APN in menuconfig");
        return modem;
    }
    // One diagnostic per boot. If the worker outlives the deadline it retains
    // valid storage and exclusive ownership of the UART until it exits.
    static diagnostic_t diagnostic;
    static StaticSemaphore_t done_storage;
    diagnostic = (diagnostic_t){
        .config = {
            .port = config->port,
            .tx_pin = config->tx_pin,
            .rx_pin = config->rx_pin,
            .baud_rate = config->baud_rate,
            .apn = CONFIG_ROADHOG_LTE_APN,
        },
        .done = xSemaphoreCreateBinaryStatic(&done_storage),
        .result = ESP_ERR_NO_MEM,
        .at_ready = true,
    };
    ESP_LOGI(TAG, "Releasing AT UART for PPP; SMS and GPS queries paused");
    modem_close(modem);
    if (xTaskCreate(run_diagnostic, "lte_diagnostic", 8192, &diagnostic, 4, NULL) == pdPASS) {
        if (xSemaphoreTake(diagnostic.done, pdMS_TO_TICKS(180000)) != pdTRUE) {
            ESP_LOGE(TAG, "Diagnostic exceeded three minutes; AT UART will stay closed");
            return NULL;
        }
    }
    ESP_LOGI(TAG, "LTE diagnostic result: %s", esp_err_to_name(diagnostic.result));
    if (!diagnostic.at_ready) {
        return NULL;
    }

    if (modem_open(config, &modem) != ESP_OK) {
        ESP_LOGE(TAG, "Could not reopen the AT UART");
        return NULL;
    }
    modem_result_t result;
    if (modem_poll(modem, 250) != ESP_OK ||
        modem_command(modem, "AT", NULL, 2000, NULL, 0, &result) != ESP_OK ||
        result.final != MODEM_FINAL_OK) {
        ESP_LOGE(TAG, "AT UART reopened but AT did not return OK");
        modem_close(modem);
        return NULL;
    }
    *uart_reopened = true;
    ESP_LOGI(TAG, "AT ready after diagnostic");
    return modem;
}
#endif
