#pragma once

#include <stdbool.h>

typedef enum {
    OTA_HEALTH_WAITING,
    OTA_HEALTH_RETRY,
    OTA_HEALTH_COMPLETE,
    OTA_HEALTH_ROLLBACK,
} ota_health_result_t;

// Resolve boot health against persistent metadata. After the deadline a
// pending candidate can only be rejected, even if initialization now succeeds.
ota_health_result_t ota_boot_health_finish(bool healthy, bool expired);
