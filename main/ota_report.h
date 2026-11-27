#pragma once

#include "ota_manifest.h"
#include "sms_control.h"
#include "esp_partition.h"

esp_err_t ota_verify_slot(const esp_partition_t *slot, const ota_manifest_t *release, int64_t deadline);
esp_err_t ota_notification_save(const ota_manifest_t *release, uint32_t address,
                                const char *owner, nvs_handle_t *store);
esp_err_t ota_notification_clear(nvs_handle_t store);
void ota_sms_report_boot(const sms_control_t *sms, modem_t *modem);
const char *ota_sms_boot_status(void);
