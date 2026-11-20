#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"

typedef struct modem modem_t;

// Called synchronously by the owner task. Copy the line if needed after return.
// Keep the callback short and do not call back into the component.
typedef void (*modem_notification_fn)(const char *line, void *context);

typedef struct {
    uart_port_t port;
    int tx_pin;
    int rx_pin;
    int baud_rate;
    modem_notification_fn on_notification;
    void *context;
} modem_config_t;

typedef enum {
    MODEM_FINAL_NONE,
    MODEM_FINAL_OK,
    MODEM_FINAL_ERROR,
} modem_final_t;

#define MODEM_LINE_CAPACITY 384
#define MODEM_COMMAND_CAPACITY 128

typedef struct {
    modem_final_t final;
    char final_line[MODEM_LINE_CAPACITY];
} modem_result_t;

// One task owns a handle and its UART. Call poll between commands to service idle RX.
esp_err_t modem_open(const modem_config_t *config, modem_t **out);
void modem_close(modem_t *modem);
esp_err_t modem_poll(modem_t *modem, uint32_t duration_ms);

// Pass a nonempty prefix for solicited payload, or NULL for a final-only command.
// Other complete lines go to the notification callback. A solicited prefix takes
// precedence if a notification and a response share that prefix.
// Matching payload is newline-delimited, with a trailing newline per line.
// Pass NULL and zero to ignore payload. The final line is returned separately.
// ESP_OK means a final was received, including modem rejection. ESP_ERR_INVALID_SIZE
// reports incomplete payload while preserving the final. After a timeout, poll
// until the pending final arrives. Until then command calls return ESP_ERR_INVALID_STATE.
esp_err_t modem_command(modem_t *modem, const char *command, const char *response_prefix,
                        uint32_t timeout_ms, char *response, size_t response_size,
                        modem_result_t *result);

// Wait for the SIMCom "> " prompt before sending printable payload plus Ctrl-Z.
// Success requires both a +CMGS acknowledgement and an OK final.
esp_err_t modem_submit_sms(modem_t *modem, unsigned tpdu_length,
                           const char *pdu, modem_result_t *result);
