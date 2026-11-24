#pragma once

#include "provisioning.h"

enum { OWNER_CODE_LENGTH = 6 };

typedef enum {
    OWNER_VERIFY_IDLE, OWNER_VERIFY_QUEUED, OWNER_VERIFY_SENDING, OWNER_VERIFY_SENT,
    OWNER_VERIFY_FAILED, OWNER_VERIFY_EXPIRED, OWNER_VERIFY_LOCKED,
} owner_verify_status_t;

typedef struct {
    char owner[OWNER_CAPACITY];
    char code[OWNER_CODE_LENGTH + 1];
} owner_verify_message_t;

// Only the authenticated setup handler may call these operations.
esp_err_t owner_verification_request(const char *owner);
void owner_verification_cancel(void);
owner_verify_status_t owner_verification_status(void);
bool owner_verification_consume(const char *owner, const char *code);

// Called serially by the normal AT loop. No HTTP handler touches the UART.
bool owner_verification_take(owner_verify_message_t *message);
void owner_verification_finish(bool accepted);
