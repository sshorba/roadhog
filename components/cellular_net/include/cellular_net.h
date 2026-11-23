#pragma once

#include <stdbool.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_netif.h"

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

// The callback runs with PPP up and the UART exclusively owned by esp_modem.
typedef esp_err_t (*cellular_operation_t)(esp_netif_t *, void *);
esp_err_t cellular_net_session(const cellular_net_config_t *, bool *at_ready,
                               cellular_operation_t, void *);
bool cellular_ppp_is_uplink(esp_netif_t *);
esp_err_t cellular_resolve_ipv4(const char *host, char *address, size_t size);

#ifdef __cplusplus
}
#endif
