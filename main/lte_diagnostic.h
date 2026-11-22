#pragma once

#include "modem.h"
#include "sdkconfig.h"

#ifdef CONFIG_ROADHOG_LTE_DIAGNOSTIC
// Once per boot. Returns NULL if the AT driver cannot safely resume.
modem_t *lte_diagnostic_run(modem_t *modem, const modem_config_t *config, bool *uart_reopened);
#endif
