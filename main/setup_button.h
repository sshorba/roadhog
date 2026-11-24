#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    SETUP_BUTTON_IDLE, SETUP_BUTTON_WAITING, SETUP_BUTTON_CONFIRMED, SETUP_BUTTON_EXPIRED,
} setup_button_state_t;

esp_err_t setup_button_init(void);
void setup_button_arm(void);
void setup_button_cancel(void);
setup_button_state_t setup_button_state(void);
