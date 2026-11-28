#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MODEM_WAITING,
    MODEM_INITIALIZING,
    MODEM_ONLINE,
    MODEM_RECOVERING,
} modem_phase_t;

typedef struct {
    modem_phase_t phase;
    int64_t retry_at;
    int64_t health_deadline;
    int64_t health_retry_at;
    unsigned health_errors;
    bool health_deadline_checked;
    bool health_finished;
    bool health_blocked;
} modem_lifecycle_t;

enum { MODEM_RETRY_MS = 5000, MODEM_BOOT_HEALTH_MS = 90000 };

bool modem_lifecycle_begin(modem_lifecycle_t *state, int64_t now);
void modem_lifecycle_result(modem_lifecycle_t *state, bool online, int64_t now);
void modem_lifecycle_check_health(modem_lifecycle_t *state, int64_t now, bool healthy);
