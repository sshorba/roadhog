#include "owner_verification.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/platform_util.h"

#define SECOND INT64_C(1000000)
#define SEND_INTERVAL (60 * SECOND)
#define SEND_WINDOW (15 * 60 * SECOND)
#define QUEUE_LIFETIME (90 * SECOND)
#define CODE_LIFETIME (5 * 60 * SECOND)
enum { SEND_LIMIT = 5, GUESS_LIMIT = 5, CODE_VALUES = 1000000 };

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static owner_verify_message_t challenge;
static owner_verify_status_t status;
static int64_t deadline, next_guess, next_send, recent_sends[SEND_LIMIT];
static unsigned send_count, guesses;
static bool in_flight;

static void clear(owner_verify_status_t next)
{
    mbedtls_platform_zeroize(&challenge, sizeof(challenge));
    status = next;
}

static void expire(int64_t now)
{
    if ((status == OWNER_VERIFY_SENT || status == OWNER_VERIFY_QUEUED) && now >= deadline) {
        clear(OWNER_VERIFY_EXPIRED);
    }
}

static void generate_code(char code[OWNER_CODE_LENGTH + 1])
{
    // Rejection sampling avoids modulo bias.
    uint32_t value;
    do {
        value = esp_random();
    } while (value >= UINT32_MAX - UINT32_MAX % CODE_VALUES);
    snprintf(code, OWNER_CODE_LENGTH + 1, "%06u", (unsigned)(value % CODE_VALUES));
}

esp_err_t owner_verification_request(const char *owner)
{
    if (!provisioning_owner_valid(owner)) {
        return ESP_ERR_INVALID_ARG;
    }
    owner_verify_message_t next = {0};
    strcpy(next.owner, owner);
    generate_code(next.code);
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    esp_err_t error = ESP_ERR_TIMEOUT;
    unsigned slot = send_count % SEND_LIMIT;
    if (in_flight) {
        error = ESP_ERR_INVALID_STATE;
    } else if (now >= next_send &&
               (send_count < SEND_LIMIT || now - recent_sends[slot] >= SEND_WINDOW) &&
               memcmp(challenge.code, next.code, sizeof(next.code))) {
        next_send = now + SEND_INTERVAL;
        recent_sends[slot] = now;
        ++send_count;
        challenge = next;
        deadline = now + QUEUE_LIFETIME;
        guesses = 0;
        next_guess = 0;
        status = OWNER_VERIFY_QUEUED;
        error = ESP_OK;
    }
    portEXIT_CRITICAL(&lock);
    mbedtls_platform_zeroize(&next, sizeof(next));
    return error;
}

void owner_verification_cancel(void)
{
    portENTER_CRITICAL(&lock);
    clear(OWNER_VERIFY_IDLE);
    // A send already running may complete, but cannot re-arm a canceled code.
    portEXIT_CRITICAL(&lock);
}

owner_verify_status_t owner_verification_status(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    expire(now);
    owner_verify_status_t current = status;
    portEXIT_CRITICAL(&lock);
    return current;
}

bool owner_verification_consume(const char *owner, const char *code)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    expire(now);
    bool confirmed = false;
    if (status == OWNER_VERIFY_SENT && now >= next_guess) {
        next_guess = now + SECOND;
        confirmed = !strcmp(challenge.owner, owner) && strlen(code) == OWNER_CODE_LENGTH &&
                    mbedtls_ct_memcmp(challenge.code, code, OWNER_CODE_LENGTH) == 0;
        if (confirmed) {
            clear(OWNER_VERIFY_IDLE);
        } else if (++guesses >= GUESS_LIMIT) {
            clear(OWNER_VERIFY_LOCKED);
        }
    }
    portEXIT_CRITICAL(&lock);
    return confirmed;
}

bool owner_verification_take(owner_verify_message_t *message)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    expire(now);
    bool queued = status == OWNER_VERIFY_QUEUED;
    if (queued) {
        *message = challenge;
        in_flight = true;
        status = OWNER_VERIFY_SENDING;
    }
    portEXIT_CRITICAL(&lock);
    return queued;
}

void owner_verification_finish(bool accepted)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    in_flight = false;
    if (status == OWNER_VERIFY_SENDING) {
        if (accepted) {
            deadline = now + CODE_LIFETIME;
            status = OWNER_VERIFY_SENT;
        } else {
            clear(OWNER_VERIFY_FAILED);
        }
    }
    portEXIT_CRITICAL(&lock);
}
