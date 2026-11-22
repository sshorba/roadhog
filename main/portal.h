#pragma once

#include <stdbool.h>

#include "connection_status.h"

esp_err_t portal_init(void);
// Data is a static label from lte_data_context(), or NULL if unknown.
void portal_set_status(lte_status_t lte, const char *data, gps_status_t gps);

// An offline fallback overrides requests to disable the AP.
esp_err_t portal_request(bool enabled);
