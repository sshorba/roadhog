#include "modem_lifecycle.h"

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

bool modem_lifecycle_finish_health(modem_lifecycle_t *state, int64_t now, bool online)
{
    if (state->health_finished || (!online && now < state->health_deadline)) {
        return false;
    }
    state->health_finished = true;
    return true;
}
