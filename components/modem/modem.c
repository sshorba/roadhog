#include "modem.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

struct modem {
    modem_config_t config;
    QueueHandle_t events;
    char line[MODEM_LINE_CAPACITY];
    char pending_command[MODEM_COMMAND_CAPACITY];
    char pending_prefix[MODEM_LINE_CAPACITY];
    size_t line_length;
    bool discard_line;
    bool waiting_for_final;
    bool uart_fault;
};

typedef struct {
    char *response;
    size_t capacity;
    size_t used;
    bool incomplete;
    bool waiting_for_prompt;
    bool prompt_seen;
    modem_result_t *result;
} transaction_t;

static modem_final_t final_code(const char *line)
{
    if (strcmp(line, "OK") == 0) {
        return MODEM_FINAL_OK;
    }
    if (strcmp(line, "ERROR") == 0 || strncmp(line, "+CME ERROR:", 11) == 0 ||
        strncmp(line, "+CMS ERROR:", 11) == 0) {
        return MODEM_FINAL_ERROR;
    }
    return MODEM_FINAL_NONE;
}

static void handle_line(modem_t *modem, transaction_t *transaction)
{
    const char *line = modem->line + strspn(modem->line, " \t");
    modem_final_t final = final_code(line);
    if (final != MODEM_FINAL_NONE) {
        if (modem->waiting_for_final && transaction) {
            transaction->result->final = final;
            strcpy(transaction->result->final_line, line);
        }
        modem->waiting_for_final = false;
        return;
    }

    if (modem->waiting_for_final) {
        if (strcmp(line, modem->pending_command) == 0) {
            return;
        }
        bool serial_number = !strcmp(modem->pending_command, "AT+CGSN") &&
                             strlen(line) == 15 && strspn(line, "0123456789") == 15;
        if (serial_number || (modem->pending_prefix[0] &&
            strncmp(line, modem->pending_prefix, strlen(modem->pending_prefix)) == 0)) {
            size_t length = strlen(line);
            if (transaction && transaction->response && !transaction->incomplete) {
                if (length + 2 > transaction->capacity - transaction->used) {
                    transaction->incomplete = true;
                    return;
                }
                char *destination = transaction->response + transaction->used;
                memcpy(destination, line, length);
                destination[length] = '\n';
                destination[length + 1] = '\0';
                transaction->used += length + 1;
            }
            return;
        }
    }

    if (modem->config.on_notification) {
        modem->config.on_notification(line, modem->config.context);
    }
}

static void consume_byte(modem_t *modem, uint8_t byte, transaction_t *transaction)
{
    if (transaction && transaction->waiting_for_prompt && !modem->discard_line &&
        modem->line_length == 1 && modem->line[0] == '>' && byte == ' ') {
        modem->line_length = 0;
        transaction->prompt_seen = true;
        return;
    }
    if (byte == '\r' || byte == '\n') {
        if (!modem->discard_line && modem->line_length > 0) {
            size_t end = modem->line_length;
            while (end && (modem->line[end - 1] == ' ' || modem->line[end - 1] == '\t')) {
                --end;
            }
            modem->line[end] = '\0';
            if (end) {
                handle_line(modem, transaction);
            }
        }
        modem->line_length = 0;
        modem->discard_line = false;
        return;
    }
    if (modem->discard_line) {
        return;
    }
    if (byte == 0 || modem->line_length == sizeof(modem->line) - 1) {
        modem->discard_line = true;
        if (transaction && modem->waiting_for_final) {
            transaction->incomplete = true;
        }
        return;
    }
    modem->line[modem->line_length++] = (char)byte;
}

static bool uart_healthy(modem_t *modem)
{
    uart_event_t event;
    while (xQueueReceive(modem->events, &event, 0) == pdTRUE) {
        if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
            event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR ||
            event.type == UART_BREAK) {
            if (!modem->uart_fault) {
                ESP_LOGE("modem", "UART%d receive fault (event %d)",
                         modem->config.port, event.type);
            }
            modem->uart_fault = true;
        }
    }
    return !modem->uart_fault;
}

static esp_err_t receive(modem_t *modem, transaction_t *transaction, uint32_t duration_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    do {
        if (!uart_healthy(modem)) {
            return ESP_FAIL;
        }
        uint8_t byte;
        int count = uart_read_bytes(modem->config.port, &byte, 1, 1);
        if (count < 0 || !uart_healthy(modem)) {
            modem->uart_fault = true;
            return ESP_FAIL;
        }
        if (count == 1) {
            if (transaction && esp_timer_get_time() >= deadline) {
                consume_byte(modem, byte, NULL);
                return ESP_ERR_TIMEOUT;
            }
            consume_byte(modem, byte, transaction);
            if (transaction && transaction->prompt_seen) {
                return ESP_OK;
            }
            if (transaction && transaction->result->final != MODEM_FINAL_NONE) {
                return transaction->incomplete ? ESP_ERR_INVALID_SIZE : ESP_OK;
            }
        }
    } while (esp_timer_get_time() < deadline);

    return transaction ? ESP_ERR_TIMEOUT : ESP_OK;
}

esp_err_t modem_open(const modem_config_t *config, modem_t **out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (!config || config->baud_rate <= 0 || config->port >= UART_NUM_MAX ||
        config->port < 0 || config->tx_pin < 0 || config->rx_pin < 0 ||
        config->tx_pin == config->rx_pin) {
        return ESP_ERR_INVALID_ARG;
    }

    modem_t *modem = calloc(1, sizeof(*modem));
    if (!modem) {
        return ESP_ERR_NO_MEM;
    }
    modem->config = *config;
    uart_config_t uart = {
        .baud_rate = config->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (uart_is_driver_installed(config->port)) {
        free(modem);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = uart_param_config(config->port, &uart);
    if (error == ESP_OK) {
        error = uart_set_pin(config->port, config->tx_pin, config->rx_pin,
                             UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (error == ESP_OK) {
        error = uart_driver_install(config->port, 2048, 0, 32, &modem->events, 0);
    }
    if (error != ESP_OK) {
        free(modem);
        return error;
    }
    *out = modem;
    return ESP_OK;
}

void modem_close(modem_t *modem)
{
    if (modem) {
        uart_driver_delete(modem->config.port);
        free(modem);
    }
}

esp_err_t modem_poll(modem_t *modem, uint32_t duration_ms)
{
    if (!modem || duration_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return receive(modem, NULL, duration_ms);
}

bool modem_maintenance_ready(modem_t *modem)
{
    return modem && receive(modem, NULL, 20) == ESP_OK &&
           !modem->waiting_for_final && !modem->line_length && !modem->discard_line;
}

static esp_err_t run_command(modem_t *modem, const char *command, const char *response_prefix,
                             uint32_t timeout_ms, char *response, size_t response_size,
                             modem_result_t *result, const char *pdu)
{
    if (!modem || !command || !result || timeout_ms == 0 ||
        (response_prefix && (!*response_prefix ||
         strnlen(response_prefix, MODEM_LINE_CAPACITY) == MODEM_LINE_CAPACITY)) || ((response == NULL) != (response_size == 0))) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = (modem_result_t){0};
    if (response) {
        response[0] = '\0';
    }

    char wire[MODEM_COMMAND_CAPACITY];
    size_t length = strnlen(command, sizeof(wire));
    if (length < 2 || length >= sizeof(wire) || strncmp(command, "AT", 2) != 0 ||
        strpbrk(command, "\r\n")) {
        return ESP_ERR_INVALID_ARG;
    }

    // Service already-buffered input before attributing any line to this command.
    esp_err_t error = receive(modem, NULL, 20);
    if (error != ESP_OK) {
        return error;
    }
    if (modem->waiting_for_final || modem->line_length || modem->discard_line) {
        return ESP_ERR_INVALID_STATE;
    }

    transaction_t transaction = {
        .response = response,
        .capacity = response_size,
        .result = result,
        .waiting_for_prompt = pdu != NULL,
    };
    strcpy(modem->pending_command, command);
    strcpy(modem->pending_prefix, response_prefix ? response_prefix : "");
    memcpy(wire, command, length);
    wire[length] = '\r';
    modem->waiting_for_final = true;
    if (uart_write_bytes(modem->config.port, wire, length + 1) != (int)(length + 1)) {
        modem->uart_fault = true;
        return ESP_FAIL;
    }
    error = receive(modem, &transaction, pdu ? 5000 : timeout_ms);
    if (!pdu) {
        return error;
    }
    if (!transaction.prompt_seen) {
        if (error == ESP_ERR_TIMEOUT) {
            // The modem may enter its input editor after this deadline. Require
            // reinitialization before allowing any more AT commands.
            modem->uart_fault = true;
        }
        return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
    }

    char payload[MODEM_LINE_CAPACITY];
    size_t payload_length = strlen(pdu);
    memcpy(payload, pdu, payload_length);
    payload[payload_length] = 0x1a;
    if (uart_write_bytes(modem->config.port, payload, payload_length + 1) !=
        (int)(payload_length + 1)) {
        modem->uart_fault = true;
        return ESP_FAIL;
    }
    transaction.waiting_for_prompt = false;
    transaction.prompt_seen = false;
    error = receive(modem, &transaction, timeout_ms);
    if (error == ESP_OK &&
        (result->final != MODEM_FINAL_OK || strncmp(response, "+CMGS:", 6) != 0)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return error;
}

esp_err_t modem_command(modem_t *modem, const char *command, const char *response_prefix,
                        uint32_t timeout_ms, char *response, size_t response_size,
                        modem_result_t *result)
{
    return run_command(modem, command, response_prefix, timeout_ms, response,
                       response_size, result, NULL);
}

// A typed response plus a final distinguishes command mode from a stale OK.
static bool command_mode_confirmed(modem_t *modem)
{
    for (unsigned attempt = 0; attempt < 3 && !modem->uart_fault; ++attempt) {
        char response[24];
        modem_result_t result;
        if (modem_command(modem, "AT+CMGF?", "+CMGF:", 2000,
                          response, sizeof(response), &result) == ESP_OK &&
            result.final == MODEM_FINAL_OK && !strncmp(response, "+CMGF:", 6)) {
            const char *value = response + 6;
            value += strspn(value, " \t");
            if ((*value == '0' || *value == '1') && !strcmp(value + 1, "\n")) {
                return true;
            }
        }
        if (attempt < 2 && receive(modem, NULL, 750) != ESP_OK) {
            break;
        }
    }
    return false;
}

static esp_err_t sync_startup(modem_t *modem)
{
    if (command_mode_confirmed(modem)) {
        return ESP_OK;
    }
    if (modem->uart_fault) {
        return ESP_FAIL;
    }

    // Startup has no live transactions to preserve. A previous ESP32 boot
    // may have left the powered modem in PPP while our UART parser timed out.
    // SIM7600 requires a quiet TX guard interval on each side of bare +++.
    uart_port_t port = modem->config.port;
    if (uart_wait_tx_done(port, pdMS_TO_TICKS(200)) != ESP_OK) {
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(1200));
    if (uart_flush_input(port) != ESP_OK) {
        return ESP_FAIL;
    }
    modem->waiting_for_final = false;
    modem->line_length = 0;
    modem->discard_line = false;
    if (uart_write_bytes(port, "+++", 3) != 3 ||
        uart_wait_tx_done(port, pdMS_TO_TICKS(200)) != ESP_OK) {
        modem->uart_fault = true;
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(1200));

    modem_result_t result = {0};
    transaction_t escape = {.result = &result};
    modem->waiting_for_final = true;
    modem->pending_command[0] = '\0';
    modem->pending_prefix[0] = '\0';
    if (receive(modem, &escape, 2000) != ESP_OK || result.final != MODEM_FINAL_OK ||
        !command_mode_confirmed(modem)) {
        return ESP_ERR_INVALID_STATE;
    }

    // Escape preserves the original data call. End it only after the
    // acknowledged escape and a solicited command-mode payload.
    if (modem_command(modem, "ATH", NULL, 10000, NULL, 0, &result) != ESP_OK ||
        result.final != MODEM_FINAL_OK || !command_mode_confirmed(modem)) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI("modem", "Recovered command mode and ended stale data call");
    return ESP_OK;
}

esp_err_t modem_sync_startup(modem_t *modem)
{
    if (!modem) {
        return ESP_ERR_INVALID_ARG;
    }
    // Do not deliver PPP fragments or stale notifications during recovery.
    modem_notification_fn notify = modem->config.on_notification;
    modem->config.on_notification = NULL;
    esp_err_t error = sync_startup(modem);
    modem->config.on_notification = notify;
    return error;
}

esp_err_t modem_submit_sms(modem_t *modem, unsigned tpdu_length, const char *pdu,
                           modem_result_t *result)
{
    if (!pdu || !tpdu_length || tpdu_length > 176) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t length = strnlen(pdu, MODEM_LINE_CAPACITY);
    if (length != 2 * (tpdu_length + 1) || strspn(pdu, "0123456789ABCDEFabcdef") != length ||
        pdu[0] != '0' || pdu[1] != '0') {
        return ESP_ERR_INVALID_ARG;
    }

    char command[24];
    char response[MODEM_LINE_CAPACITY];
    snprintf(command, sizeof(command), "AT+CMGS=%u", tpdu_length);
    return run_command(modem, command, "+CMGS:", 60000, response, sizeof(response), result, pdu);
}
