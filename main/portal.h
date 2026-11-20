#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t portal_init(void);
void portal_set_cellular(bool registered);

// An offline fallback overrides requests to disable the AP.
esp_err_t portal_request(bool enabled);
