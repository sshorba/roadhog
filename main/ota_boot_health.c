#include "ota_boot_health.h"
#include "sdkconfig.h"

#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#include <stddef.h>

#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "ota_health";

#ifdef CONFIG_ROADHOG_OTA_BENCH_BOOT_PEER
#include "esp_system.h"

static void boot_untried_peer(void)
{
    const esp_partition_t *peer = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                            ESP_PARTITION_SUBTYPE_APP_OTA_1,
                                                            NULL);
    esp_ota_img_states_t peer_state;
    if (!peer || esp_ota_get_state_partition(peer, &peer_state) != ESP_ERR_NOT_FOUND) {
        return;
    }
    // This bench-only path selects an already staged image through the public
    // API, which verifies its format and creates a NEW otadata entry.
    esp_err_t error = esp_ota_set_boot_partition(peer);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Bench peer was not selected: %s", esp_err_to_name(error));
        return;
    }
    ESP_LOGI(TAG, "Bench selecting untried ota_1 and rebooting");
    esp_restart();
}
#endif

void ota_boot_health_finish(bool healthy)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGE(TAG, "Running partition unavailable");
        return;
    }
    esp_ota_img_states_t state;
    esp_err_t error = esp_ota_get_state_partition(running, &state);
    if (error == ESP_ERR_NOT_FOUND) {
        if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
            ESP_LOGW(TAG, "Slot A has no persisted OTA state; peer selection is blocked");
        }
        return;
    }
    if (error == ESP_ERR_NOT_SUPPORTED) {
        return;
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Unable to read running OTA state: %s", esp_err_to_name(error));
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "%s pending verification", running->label);
#ifdef CONFIG_ROADHOG_OTA_BENCH_REJECT_PENDING
        healthy = false;
        ESP_LOGW(TAG, "Bench forced health failure");
#endif
        if (!healthy) {
            if (!esp_ota_check_rollback_is_possible()) {
                ESP_LOGE(TAG, "Startup failed and no rollback image is available; keeping pending state");
                return;
            }
            ESP_LOGW(TAG, "Startup failed; requesting rollback");
            error = esp_ota_mark_app_invalid_rollback_and_reboot();
            // A successful request reboots inside esp_ota.
            ESP_LOGE(TAG, "Rollback API returned: %s", esp_err_to_name(error));
            return;
        }
        error = esp_ota_mark_app_valid_cancel_rollback();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Could not confirm %s: %s", running->label, esp_err_to_name(error));
            return;
        }
        ESP_LOGI(TAG, "%s marked valid after startup checks", running->label);
        error = esp_ota_get_state_partition(running, &state);
        if (error != ESP_OK || state != ESP_OTA_IMG_VALID) {
            ESP_LOGE(TAG, "Could not read back VALID state for %s", running->label);
            return;
        }
    }

    if (healthy && state == ESP_OTA_IMG_VALID &&
        running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
        // A may be running after a bootloader fallback while another slot is
        // still selected in otadata. Require A to own the selected entry so
        // scheduling B writes the other sector and leaves A's entry intact.
        const esp_partition_t *selected = esp_ota_get_boot_partition();
        if (!selected || selected->address != running->address) {
            ESP_LOGW(TAG, "Slot A is running but not selected; peer selection is blocked");
            return;
        }
        ESP_LOGI(TAG, "Slot A startup checks passed; selected OTA state is VALID");
#ifdef CONFIG_ROADHOG_OTA_BENCH_BOOT_PEER
        boot_untried_peer();
#endif
    }
}
#else
void ota_boot_health_finish(bool healthy)
{
    (void)healthy;
}
#endif
