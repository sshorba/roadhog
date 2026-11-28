#include "modem_lifecycle.h"
#include "ota_boot_health.h"

bool modem_lifecycle_begin(modem_lifecycle_t *state, int64_t now)
{
    if ((state->phase != MODEM_WAITING && state->phase != MODEM_RECOVERING) ||
        now < state->retry_at) {
        return false;
    }
    state->phase = MODEM_INITIALIZING;
    return true;
}

void modem_lifecycle_result(modem_lifecycle_t *state, bool online, int64_t now)
{
    state->phase = online ? MODEM_ONLINE : MODEM_RECOVERING;
    state->retry_at = now + MODEM_RETRY_MS;
}

void modem_lifecycle_check_health(modem_lifecycle_t *state, int64_t now, bool healthy)
{
    enum { MAX_ERRORS = 3, HEALTH_RETRY_MS = 1000 };
    bool expired = now >= state->health_deadline;
    if (expired && !state->health_deadline_checked && !state->health_finished) {
        // Give a still-pending image a separate, bounded rejection window.
        state->health_blocked = false;
        state->health_errors = 0;
    }
    if (state->health_finished || state->health_blocked ||
        (now < state->health_retry_at && (!expired || state->health_deadline_checked))) {
        return;
    }

    state->health_deadline_checked = expired;
    ota_health_result_t result = ota_boot_health_finish(healthy, expired);
    if (result == OTA_HEALTH_COMPLETE || result == OTA_HEALTH_ROLLBACK) {
        state->health_finished = true;
    } else if (result == OTA_HEALTH_RETRY) {
        state->health_blocked = ++state->health_errors >= MAX_ERRORS;
        state->health_retry_at = now + HEALTH_RETRY_MS;
    }
}
