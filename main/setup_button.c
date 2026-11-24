#include "setup_button.h"

#include <stdint.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

enum { BUTTON_PIN = GPIO_NUM_4, SAMPLE_US = 10000, DEBOUNCE_US = 40000, WINDOW_US = 30000000 };
_Static_assert(CONFIG_ROADHOG_STATUS_LED_GPIO != BUTTON_PIN, "GPIO4 is reserved for setup confirmation");

static portMUX_TYPE button_lock = portMUX_INITIALIZER_UNLOCKED;
static setup_button_state_t button_state;
static int64_t expires, level_since;
static bool sampled, last_high, released;

static void expire_window(int64_t now)
{
    if (button_state == SETUP_BUTTON_WAITING && now >= expires) {
        button_state = SETUP_BUTTON_EXPIRED;
    }
}

static void sample_button(void *unused)
{
    (void)unused;
    portENTER_CRITICAL(&button_lock);
    int64_t now = esp_timer_get_time();
    expire_window(now);
    if (button_state == SETUP_BUTTON_WAITING) {
        bool high = gpio_get_level(BUTTON_PIN);
        if (!sampled || high != last_high) {
            sampled = true;
            last_high = high;
            level_since = now;
        } else if (now - level_since >= DEBOUNCE_US) {
            if (high) {
                released = true;
            } else if (released) {
                button_state = SETUP_BUTTON_CONFIRMED;
            }
        }
    }
    portEXIT_CRITICAL(&button_lock);
}

esp_err_t setup_button_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = UINT64_C(1) << BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&config);
    if (error != ESP_OK) {
        return error;
    }
    const esp_timer_create_args_t timer_config = {
        .callback = sample_button, .name = "setup_btn", .skip_unhandled_events = true,
    };
    esp_timer_handle_t timer;
    error = esp_timer_create(&timer_config, &timer);
    if (error != ESP_OK) {
        return error;
    }
    error = esp_timer_start_periodic(timer, SAMPLE_US);
    if (error != ESP_OK) {
        esp_timer_delete(timer);
    }
    return error;
}

void setup_button_arm(void)
{
    portENTER_CRITICAL(&button_lock);
    expires = esp_timer_get_time() + WINDOW_US;
    sampled = released = false;
    button_state = SETUP_BUTTON_WAITING;
    portEXIT_CRITICAL(&button_lock);
}

void setup_button_cancel(void)
{
    portENTER_CRITICAL(&button_lock);
    button_state = SETUP_BUTTON_IDLE;
    portEXIT_CRITICAL(&button_lock);
}

setup_button_state_t setup_button_state(void)
{
    portENTER_CRITICAL(&button_lock);
    expire_window(esp_timer_get_time());
    setup_button_state_t current = button_state;
    portEXIT_CRITICAL(&button_lock);
    return current;
}
