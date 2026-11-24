#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

enum {
    OWNER_CAPACITY = 17, APN_CAPACITY = 101, SETUP_SECRET_LENGTH = 32,
    ADMIN_PASSWORD_MIN = 12, ADMIN_PASSWORD_MAX = 64,
};

typedef enum { PROVISION_CORRUPT, PROVISION_FACTORY, PROVISION_CONFIGURED } provisioning_state_t;

typedef struct {
    uint8_t salt[16];
    uint8_t hash[32];
} password_record_t;

void provisioning_init(void);
provisioning_state_t provisioning_state(void);
const char *provisioning_owner(void);
const char *provisioning_apn(void);
const char *provisioning_wifi_password(void);
const password_record_t *provisioning_admin(void);
esp_err_t provisioning_save_admin(const password_record_t *admin);
const char *provisioning_start_session(void);
bool provisioning_setup_completed(const char *token);
void provisioning_cancel_claim(void);
esp_err_t provisioning_finish_claim(const char *token, const char **wifi_password);
bool provisioning_setup_authorized(const char *token);
esp_err_t provisioning_claim(const char *token, const char *owner, const char *apn,
                             const char *code, const password_record_t *admin);
bool provisioning_owner_valid(const char *owner);
