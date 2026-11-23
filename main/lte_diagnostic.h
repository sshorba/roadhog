#pragma once

#include "modem.h"
#include "sdkconfig.h"

#ifdef CONFIG_ROADHOG_LTE_DIAGNOSTIC
#include "cellular_net.h"

// Serial calls only. An operation context must remain valid after a timeout.
modem_t *lte_session_run(modem_t *, const modem_config_t *, bool *,
                         cellular_operation_t, void *, esp_err_t *);

// Once per boot. Returns NULL if the AT driver cannot safely resume.
modem_t *lte_diagnostic_run(modem_t *modem, const modem_config_t *config, bool *uart_reopened);
#endif
