#include "sdkconfig.h"
#include "lte_diagnostic.h"
#include "provisioning.h"

#ifdef CONFIG_ROADHOG_LTE_DIAGNOSTIC

#include "cellular_net.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "lte_diagnostic";

enum {
    LTE_DIAGNOSTIC_STACK_BYTES = 8 * 1024,
    OTA_SESSION_STACK_BYTES = 24 * 1024,
    STACK_MARGIN_BYTES = 2 * 1024,
};

typedef struct {
    cellular_net_config_t config;
    SemaphoreHandle_t done;
    esp_err_t result;
    bool at_ready;
    cellular_operation_t operation;
    void *context;
} diagnostic_t;

static void run_diagnostic(void *context)
{
    diagnostic_t *diagnostic = context;
    diagnostic->result = cellular_net_session(&diagnostic->config, &diagnostic->at_ready,
                                              diagnostic->operation, diagnostic->context);
    unsigned remaining = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "%s minimum free stack: %u bytes",
             diagnostic->operation ? "OTA session" : "LTE diagnostic", remaining);
    if (remaining < STACK_MARGIN_BYTES) {
        ESP_LOGW(TAG, "LTE worker stack margin is below %u bytes", (unsigned)STACK_MARGIN_BYTES);
    }
    xSemaphoreGive(diagnostic->done);
    vTaskDelete(NULL);
}

// Only the application UART owner accesses this handle. The worker releases
// cellular resources before signaling and never touches the UART afterward.
static SemaphoreHandle_t worker_done;

bool lte_session_owns_uart(void)
{
    if (worker_done && xSemaphoreTake(worker_done, 0) == pdTRUE) {
        worker_done = NULL;
    }
    return worker_done != NULL;
}

modem_t *lte_session_run(modem_t *modem, const modem_config_t *config, bool *uart_reopened,
                         cellular_operation_t operation, void *context, esp_err_t *session_result)
{
    *uart_reopened = false;
    *session_result = ESP_ERR_INVALID_ARG;
    if (lte_session_owns_uart()) {
        *session_result = ESP_ERR_INVALID_STATE;
        return modem;
    }
    const char *apn = provisioning_apn();
    if (!apn || !cellular_apn_valid(apn)) {
        ESP_LOGE(TAG, "Provision a valid carrier APN");
        return modem;
    }
    // Calls are serialized by the AT owner. If a worker outlives its deadline it retains
    // valid storage and exclusive ownership of the UART until it exits.
    static diagnostic_t diagnostic;
    static StaticSemaphore_t done_storage;
    diagnostic = (diagnostic_t){
        .config = {
            .port = config->port,
            .tx_pin = config->tx_pin,
            .rx_pin = config->rx_pin,
            .baud_rate = config->baud_rate,
            .apn = apn,
        },
        .done = xSemaphoreCreateBinaryStatic(&done_storage),
        .result = ESP_ERR_NO_MEM,
        .at_ready = true,
        .operation = operation,
        .context = context,
    };
    ESP_LOGI(TAG, "Releasing AT UART for PPP; SMS and GPS queries paused");
    modem_close(modem);
    const char *worker_name = operation ? "ota_session" : "lte_diagnostic";
    unsigned stack_bytes = operation ? OTA_SESSION_STACK_BYTES : LTE_DIAGNOSTIC_STACK_BYTES;
    if (xTaskCreate(run_diagnostic, worker_name, stack_bytes, &diagnostic, 4, NULL) == pdPASS) {
        if (xSemaphoreTake(diagnostic.done, pdMS_TO_TICKS(operation ? 480000 : 180000)) != pdTRUE) {
            worker_done = diagnostic.done;
            ESP_LOGE(TAG, "LTE session deadline exceeded; waiting for worker to release UART");
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
    modem_watch_restart(modem);
    modem_result_t result;
    char response[32];
    if (modem_poll(modem, 250) != ESP_OK ||
        modem_command(modem, "AT+CMGF?", "+CMGF:", 2000, response, sizeof(response), &result) != ESP_OK ||
        result.final != MODEM_FINAL_OK || !response[0]) {
        ESP_LOGE(TAG, "AT UART reopened but AT did not return OK");
        modem_close(modem);
        return NULL;
    }
    *session_result = diagnostic.result;
    *uart_reopened = true;
    ESP_LOGI(TAG, "AT ready after diagnostic");
    return modem;
}
modem_t *lte_diagnostic_run(modem_t *modem, const modem_config_t *config, bool *uart_reopened)
{
    esp_err_t result;
    return lte_session_run(modem, config, uart_reopened, NULL, NULL, &result);
}
#endif
