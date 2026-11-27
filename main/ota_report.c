#include "ota_report.h"
#include "sdkconfig.h"

#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && defined(CONFIG_ROADHOG_SMS_CONTROL)
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "psa/crypto.h"

#define IMAGE_PREFIX (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

#ifdef ROADHOG_SMS_OTA_DESCRIPTOR
// ESP-IDF puts this immediately after esp_app_desc_t in the application image.
__attribute__((section(".rodata_custom_desc"), used, aligned(1)))
const char roadhog_sms_ota_descriptor[] = OTA_SMS_IMAGE_MARKER;
#endif

static const char *REPORT_TAG = "ota_report";
static const char *boot_status = "No OTA request this boot.";

typedef struct {
    ota_manifest_t release;
    uint32_t address;
    char owner[SMS_NUMBER_CAPACITY];
} notification_t;

esp_err_t ota_verify_slot(const esp_partition_t *slot, const ota_manifest_t *manifest,
                          int64_t deadline)
{
    enum { READ_SIZE = 1024 };
    uint8_t *buffer = malloc(READ_SIZE);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        free(buffer);
        return ESP_FAIL;
    }
    esp_err_t error = ESP_OK;
    for (size_t offset = 0; offset < manifest->size; ) {
        size_t remaining = manifest->size - offset;
        size_t length = remaining < READ_SIZE ? remaining : READ_SIZE;
        if (esp_timer_get_time() >= deadline) {
            error = ESP_ERR_TIMEOUT;
            break;
        }
        error = esp_partition_read(slot, offset, buffer, length);
        if (error != ESP_OK) {
            break;
        }
        if (psa_hash_update(&hash, buffer, length) != PSA_SUCCESS) {
            error = ESP_FAIL;
            break;
        }
        offset += length;
    }
    size_t length;
    uint8_t digest[32];
    if (error == ESP_OK) {
        if (psa_hash_finish(&hash, digest, sizeof(digest), &length) != PSA_SUCCESS ||
            length != sizeof(digest)) {
            error = ESP_FAIL;
        } else if (memcmp(digest, manifest->digest, sizeof(digest))) {
            error = ESP_ERR_INVALID_CRC;
        }
    }
    psa_hash_abort(&hash);
    free(buffer);
    return error;
}

static esp_err_t notification_read(nvs_handle_t store, notification_t *record)
{
    size_t length = sizeof(*record);
    esp_err_t error = nvs_get_blob(store, "pending_v1", record, &length);
    if (error == ESP_OK &&
        (length != sizeof(*record) || !record->release.sequence ||
         record->release.size < IMAGE_PREFIX || record->release.size > OTA_SLOT_SIZE ||
         !memchr(record->owner, 0, sizeof(record->owner)))) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return error;
}

esp_err_t ota_notification_clear(nvs_handle_t store)
{
    esp_err_t error = nvs_erase_key(store, "pending_v1");
    return error == ESP_OK ? nvs_commit(store) : error;
}

#if defined(CONFIG_ROADHOG_OTA_SMS_BENCH)
esp_err_t ota_notification_save(const ota_manifest_t *release, uint32_t address,
                                const char *owner, nvs_handle_t *store)
{
    esp_err_t error = nvs_open("sms_ota", NVS_READWRITE, store);
    if (error != ESP_OK) {
        return error;
    }
    notification_t record;
    error = notification_read(*store, &record);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        record = (notification_t){.release = *release, .address = address};
        snprintf(record.owner, sizeof(record.owner), "%s", owner);
        error = nvs_set_blob(*store, "pending_v1", &record, sizeof(record));
        if (error == ESP_OK) {
            error = nvs_commit(*store);
        }
    } else if (error == ESP_OK) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error != ESP_OK) {
        nvs_close(*store);
    }
    return error;
}

#endif

static esp_err_t claim_notification(void *context)
{
    const nvs_handle_t *store = context;
    return ota_notification_clear(*store);
}

void ota_sms_report_boot(const sms_control_t *sms, modem_t *modem)
{
    if (!sms->configured || sms->routing_recovery || !sms->owner) {
        return;
    }
    nvs_handle_t store;
    if (nvs_open("sms_ota", NVS_READONLY, &store) != ESP_OK) {
        return;
    }
    notification_t record;
    if (notification_read(store, &record) != ESP_OK) {
        nvs_close(store);
        return;
    }
    nvs_close(store);
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    uint32_t current;
    if (!running || esp_ota_get_boot_partition() != running ||
        esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_VALID ||
        !ota_sequence_parse(esp_app_get_description()->version, &current)) {
        return;
    }
    bool success = false;
    if (running->address == record.address && current == record.release.sequence) {
        esp_err_t verified = ota_verify_slot(running, &record.release,
                                             esp_timer_get_time() + INT64_C(15000000));
        if (verified != ESP_OK && verified != ESP_ERR_INVALID_CRC) {
            return;
        }
        success = verified == ESP_OK;
    }
    if (!modem_maintenance_ready(modem) || sms->pending_acks || sms->pending_nacks || sms->delivery_fault || sms->expected_tpdu ||
        nvs_open("sms_ota", NVS_READWRITE, &store) != ESP_OK) {
        return;
    }
    char reply[SMS_TEXT_CAPACITY];
    if (success) {
        snprintf(reply, sizeof(reply), "Updated successfully to v%" PRIu32
                 ". SMS ready and LTE registered.", current);
        boot_status = "Update validated.";
    } else {
        snprintf(reply, sizeof(reply), "Running v%" PRIu32 ". Update to v%" PRIu32
                 " did not validate. Rollback or interruption possible.", current, record.release.sequence);
        boot_status = "Requested update did not validate.";
    }
    esp_err_t error;
    if (strcmp(record.owner, sms->owner) != 0) {
        error = ota_notification_clear(store);
    } else {
        error = sms_control_send_claimed(modem, sms->owner, reply, claim_notification, &store);
    }
    nvs_close(store);
    if (error != ESP_OK) {
        ESP_LOGW(REPORT_TAG, "Boot report send or journal claim did not complete");
    }
}

const char *ota_sms_boot_status(void)
{
    return boot_status;
}
#endif
