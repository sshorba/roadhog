#pragma once

#include <stdbool.h>

#include "driver/uart.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uart_port_t port;
    int tx_pin;
    int rx_pin;
    int baud_rate;
    const char *apn;
} cellular_net_config_t;

bool cellular_apn_valid(const char *apn);

// Caller releases the AT UART before this call. The driver is released on
// return. at_ready reports whether the modem can safely be handed back to AT.
esp_err_t cellular_net_test(const cellular_net_config_t *config, bool *at_ready);

#ifdef __cplusplus
}
#endif
