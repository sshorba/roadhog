#include "ota_boot_health.h"
#include "sdkconfig.h"

#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#include <stddef.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

static const char *TAG = "ota_health";

#ifdef CONFIG_ROADHOG_OTA_BENCH_BOOT_PEER

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

static ota_health_result_t restart_after_rollback(const esp_partition_t *running)
{
    const esp_partition_t *selected = esp_ota_get_boot_partition();
    if (!selected || selected->address == running->address) {
        ESP_LOGE(TAG, "Rollback destination could not be verified");
        return OTA_HEALTH_RETRY;
    }
    esp_restart();
    return OTA_HEALTH_ROLLBACK;
}

ota_health_result_t ota_boot_health_finish(bool healthy, bool expired)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGE(TAG, "Running partition unavailable");
        return OTA_HEALTH_RETRY;
    }
    esp_ota_img_states_t state;
    esp_err_t error = esp_ota_get_state_partition(running, &state);
    if (error == ESP_ERR_NOT_SUPPORTED) {
        return OTA_HEALTH_COMPLETE;
    }
    if (error != ESP_OK) {
        // The SDK also returns NOT_FOUND when it cannot read otadata.
        ESP_LOGW(TAG, "Unable to read running OTA state: %s", esp_err_to_name(error));
        return OTA_HEALTH_RETRY;
    }
    if (state == ESP_OTA_IMG_INVALID) {
        // A previous rollback write may have worked despite an API/read error.
        return restart_after_rollback(running);
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (!healthy && !expired) {
            return OTA_HEALTH_WAITING;
        }
        bool accept = healthy && !expired;
#ifdef CONFIG_ROADHOG_OTA_BENCH_REJECT_PENDING
        accept = false;
#endif
        const esp_partition_t *selected = esp_ota_get_boot_partition();
        if (!selected || selected->address != running->address ||
            (!accept && !esp_ota_check_rollback_is_possible())) {
            ESP_LOGE(TAG, "Startup health decision has no safe OTA metadata target");
            return OTA_HEALTH_RETRY;
        }
        error = accept ? esp_ota_mark_app_valid_cancel_rollback() :
                         esp_ota_mark_app_invalid_rollback();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "OTA health metadata write failed: %s", esp_err_to_name(error));
            return OTA_HEALTH_RETRY;
        }
        error = esp_ota_get_state_partition(running, &state);
        if (error != ESP_OK || state != (accept ? ESP_OTA_IMG_VALID : ESP_OTA_IMG_INVALID)) {
            ESP_LOGE(TAG, "OTA health metadata readback failed");
            return OTA_HEALTH_RETRY;
        }
        if (!accept) {
            return restart_after_rollback(running);
        }
        ESP_LOGI(TAG, "%s marked valid after startup checks", running->label);
    }
    if (state != ESP_OTA_IMG_VALID) {
        return OTA_HEALTH_RETRY;
    }
#ifdef CONFIG_ROADHOG_OTA_BENCH_BOOT_PEER
    if (!healthy && !expired) {
        return OTA_HEALTH_WAITING;
    }
#endif
    if (healthy && state == ESP_OTA_IMG_VALID &&
        running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
        // A may be running after a bootloader fallback while another slot is
        // still selected in otadata. Require A to own the selected entry so
        // scheduling B writes the other sector and leaves A's entry intact.
        const esp_partition_t *selected = esp_ota_get_boot_partition();
        if (!selected || selected->address != running->address) {
            ESP_LOGW(TAG, "Slot A is running but not selected; peer selection is blocked");
            return OTA_HEALTH_COMPLETE;
        }
        ESP_LOGI(TAG, "Slot A startup checks passed; selected OTA state is VALID");
#ifdef CONFIG_ROADHOG_OTA_BENCH_BOOT_PEER
        boot_untried_peer();
#endif
    }
    return OTA_HEALTH_COMPLETE;
}
#else
ota_health_result_t ota_boot_health_finish(bool healthy, bool expired)
{
    (void)healthy;
    (void)expired;
    return OTA_HEALTH_COMPLETE;
}
#endif
