#include "status_led.h"

#include <stdatomic.h>
#include <stdint.h>

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

enum { MODEM = 1, LTE = 2, GPS = 4, AP = 8 };
enum {
    OFF = 0x000000,
    RED = 0x200000,
    YELLOW = 0x201000,
    GREEN = 0x002000,
    BLUE = 0x000020,
};
enum { FRAME_MS = 400, RMT_HZ = 10000000 };

static const char *TAG = "status_led";
static atomic_uint connections;
static atomic_bool ap_enabled;
static rmt_channel_handle_t channel;
static rmt_encoder_handle_t encoder;

void status_led_set_connections(bool modem_reachable, bool lte_registered, bool gps_fix)
{
    atomic_store(&connections, (modem_reachable ? MODEM : 0) |
                               (lte_registered ? LTE : 0) | (gps_fix ? GPS : 0));
}

void status_led_set_ap(bool enabled)
{
    atomic_store(&ap_enabled, enabled);
}

static uint32_t frame_color(unsigned state, unsigned frame)
{
    if (!(state & MODEM)) {
        return RED;
    }
    if ((state & AP) && ((state & (LTE | GPS)) == (LTE | GPS) || frame >= 4)) {
        return frame % 2 == 0 ? GREEN : BLUE;
    }
    switch (state & (LTE | GPS)) {
    case LTE | GPS: return GREEN;
    case LTE: return frame % 2 == 0 ? YELLOW : OFF;
    case GPS: return frame % 2 == 0 ? RED : OFF;
    default: return frame % 2 == 0 ? YELLOW : RED;
    }
}

static esp_err_t show_color(uint32_t rgb)
{
    // The addressable LED expects green, red, then blue, most significant bit first.
    uint32_t grb = ((rgb & 0x00ff00) << 8) | ((rgb & 0xff0000) >> 8) | (rgb & 0xff);
    const rmt_symbol_word_t zero = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9};
    const rmt_symbol_word_t one = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3};
    rmt_symbol_word_t symbols[25];
    for (unsigned bit = 0; bit < 24; bit++) {
        symbols[bit] = (grb & (1U << (23 - bit))) ? one : zero;
    }
    symbols[24] = (rmt_symbol_word_t){.duration0 = 1500, .duration1 = 1500};
    const rmt_transmit_config_t tx = {0};
    esp_err_t error = rmt_transmit(channel, encoder, symbols, sizeof(symbols), &tx);
    return error == ESP_OK ? rmt_tx_wait_all_done(channel, -1) : error;
}

static void status_task(void *context)
{
    (void)context;
    unsigned previous = ~0U;
    unsigned frame = 0;
    uint32_t displayed = UINT32_MAX;

    for (;;) {
        unsigned state = atomic_load(&connections) | (atomic_load(&ap_enabled) ? AP : 0);
        if (state != previous) {
            frame = 0;
            previous = state;
        }
        uint32_t color = frame_color(state, frame);
        if (color != displayed) {
            esp_err_t error = show_color(color);
            if (error == ESP_OK) {
                displayed = color;
            } else {
                ESP_LOGW(TAG, "RGB update failed: %s", esp_err_to_name(error));
            }
        }
        frame = (frame + 1) % 8;
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
}

esp_err_t status_led_start(void)
{
    const rmt_tx_channel_config_t config = {
        .gpio_num = CONFIG_ROADHOG_STATUS_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    esp_err_t error = rmt_new_tx_channel(&config, &channel);
    if (error != ESP_OK) {
        return error;
    }
    const rmt_copy_encoder_config_t copy = {};
    error = rmt_new_copy_encoder(&copy, &encoder);
    if (error == ESP_OK) {
        error = rmt_enable(channel);
        if (error == ESP_OK) {
            if (xTaskCreate(status_task, "status_led", 3072, NULL, 3, NULL) == pdPASS) {
                ESP_LOGI(TAG, "Onboard RGB LED on GPIO%d", CONFIG_ROADHOG_STATUS_LED_GPIO);
                return ESP_OK;
            }
            rmt_disable(channel);
            error = ESP_ERR_NO_MEM;
        }
        rmt_del_encoder(encoder);
    }
    rmt_del_channel(channel);
    return error;
}
