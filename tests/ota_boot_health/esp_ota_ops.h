#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_NOT_FOUND, ESP_ERR_NOT_SUPPORTED, ESP_ERR_INVALID_ARG,
       ESP_ERR_OTA_VALIDATE_FAILED, ESP_ERR_OTA_ROLLBACK_FAILED };
enum { ESP_PARTITION_TYPE_APP, ESP_PARTITION_TYPE_DATA };
enum { ESP_PARTITION_SUBTYPE_APP_FACTORY = 0, ESP_PARTITION_SUBTYPE_DATA_OTA = 0,
       ESP_PARTITION_SUBTYPE_APP_OTA_MIN = 16,
       ESP_PARTITION_SUBTYPE_APP_OTA_0 = ESP_PARTITION_SUBTYPE_APP_OTA_MIN,
       ESP_PARTITION_SUBTYPE_APP_OTA_1 };
typedef uint8_t esp_partition_subtype_t;
typedef struct {
    int type;
    esp_partition_subtype_t subtype;
    const char *label;
    uint32_t address, size, erase_size;
} esp_partition_t;
typedef enum { ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID,
               ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED }
    esp_ota_img_states_t;

const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_boot_partition(void);
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *state);
bool esp_ota_check_rollback_is_possible(void);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition);
