#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "provisioning.h"

typedef unsigned nvs_handle_t;
enum { NVS_READONLY, NVS_READWRITE };
static uint8_t stored[512];
static size_t stored_length;
static bool config_exists, legacy_sms, legacy_admin;
static bool init_error, read_error, set_error, commit_error;
static unsigned writes;
static uint8_t entropy;

static esp_err_t nvs_flash_init(void) { return init_error ? ESP_FAIL : ESP_OK; }
static void bootloader_random_enable(void) {}
static void bootloader_random_disable(void) {}

static void esp_fill_random(uint8_t *dst, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        dst[i] = ++entropy;
    }
}

static int mbedtls_ct_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static void mbedtls_platform_zeroize(void *dst, size_t n) { memset(dst, 0, n); }

static esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    if (!strcmp(name, "device_cfg")) {
        config_exists |= mode == NVS_READWRITE;
        *handle = 1;
        return config_exists ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
    }
    assert(mode == NVS_READONLY);
    if ((!strcmp(name, "sms") && legacy_sms) || (!strcmp(name, "portal_auth") && legacy_admin)) {
        *handle = 2;
        return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

static void nvs_close(nvs_handle_t handle) { assert(handle == 1 || handle == 2); }

static esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *dst, size_t *length)
{
    assert(handle == 1 && !strcmp(key, "settings_v1"));
    if (read_error) {
        return ESP_FAIL;
    }
    if (!stored_length) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (*length >= stored_length) {
        memcpy(dst, stored, stored_length);
    }
    *length = stored_length;
    return ESP_OK;
}

static esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *input, size_t length)
{
    assert(handle == 1 && !strcmp(key, "settings_v1") && length <= sizeof(stored));
    ++writes;
    if (set_error) {
        return ESP_FAIL;
    }
    memcpy(stored, input, length);
    stored_length = length;
    return ESP_OK;
}

static esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    return commit_error ? ESP_FAIL : ESP_OK;
}

#include "provisioning_under_test.inc"

enum { SMS_NUMBER_CAPACITY = OWNER_CAPACITY, SMS_USER_LIMIT = 6 };
typedef struct {
    const char *owner;
    char users[SMS_USER_LIMIT][SMS_NUMBER_CAPACITY];
    nvs_handle_t users_store;
} sms_control_t;

#include "sms_authorization_under_test.inc"

static void reboot(void)
{
    state = PROVISION_CORRUPT;
    claim_locked = false;
    memset(&record, 0, sizeof(record));
    memset(setup_token, 0, sizeof(setup_token));
    provisioning_init();
}

static void fresh_flash(void)
{
    config_exists = legacy_sms = legacy_admin = false;
    init_error = read_error = set_error = commit_error = false;
    stored_length = 0;
    writes = 0;
    reboot();
}

int main(void)
{
    const password_record_t admin = {.salt = {1}, .hash = {2}};
    const password_record_t updated = {.salt = {3}, .hash = {4}};
    char token[SETUP_SECRET_LENGTH + 1];
    char ap_key[SETUP_SECRET_LENGTH + 1];
    fresh_flash();
    assert(provisioning_state() == PROVISION_FACTORY);
    assert(!provisioning_admin() && !provisioning_owner() && !provisioning_apn());
    sms_control_t sms = {.owner = provisioning_owner(), .users = {"+5678"}};
    assert(!sms_sender_allowed(&sms, "+5678"));
    assert(strlen(provisioning_wifi_password()) == SETUP_SECRET_LENGTH);
    strcpy(token, setup_token);
    strcpy(ap_key, provisioning_wifi_password());
    assert(strcmp(token, ap_key));
    assert(provisioning_claim(ap_key, "+1234", "example.apn", &admin) == ESP_ERR_INVALID_STATE);
    assert(provisioning_claim(token, "1234", "example.apn", &admin) == ESP_ERR_INVALID_ARG);
    assert(provisioning_claim(token, "+1234", "x\rAT", &admin) == ESP_ERR_INVALID_ARG);
    assert(!writes);
    reboot();
    assert(provisioning_state() == PROVISION_FACTORY && !provisioning_setup_authorized(token));
    strcpy(token, setup_token);
    strcpy(ap_key, provisioning_wifi_password());
    assert(provisioning_claim(token, "+1234", "example.apn", &admin) == ESP_OK);
    assert(!provisioning_owner());
    assert(provisioning_claim(token, "+5678", "example.apn", &admin) == ESP_ERR_INVALID_STATE);
    assert(writes == 1);
    reboot();
    assert(provisioning_state() == PROVISION_CONFIGURED);
    assert(!strcmp(provisioning_owner(), "+1234") && !strcmp(provisioning_apn(), "example.apn"));
    sms.owner = provisioning_owner();
    assert(sms_sender_allowed(&sms, "+1234") && sms_sender_allowed(&sms, "+5678"));
    assert(!sms_sender_allowed(&sms, "+9876") && !sms_sender_allowed(&sms, ""));
    assert(!strcmp(change_user(&sms, "+5678", "+9876", true), "Owner only"));
    assert(!strcmp(change_user(&sms, "+1234", "+1234", false), "Owner is permanent"));
    assert(!strcmp(provisioning_wifi_password(), ap_key));
    assert(!provisioning_setup_authorized(token));
    assert(provisioning_save_admin(&updated) == ESP_OK);
    reboot();
    assert(!memcmp(provisioning_admin(), &updated, sizeof(updated)));
    assert(!strcmp(provisioning_owner(), "+1234"));
    stored[0] = 99;
    reboot();
    assert(provisioning_state() == PROVISION_CORRUPT && !provisioning_wifi_password());
    stored[0] = 1;
    --stored_length;
    reboot();
    assert(provisioning_state() == PROVISION_CORRUPT);
    ++stored_length;
    read_error = true;
    reboot();
    assert(provisioning_state() == PROVISION_CORRUPT);

    fresh_flash();
    legacy_sms = true;
    reboot();
    assert(provisioning_state() == PROVISION_CORRUPT && !provisioning_owner() && !writes);
    legacy_sms = false;
    legacy_admin = true;
    reboot();
    assert(provisioning_state() == PROVISION_CORRUPT && !provisioning_admin() && !writes);
    init_error = true;
    reboot();
    assert(provisioning_state() == PROVISION_CORRUPT);

    fresh_flash();
    set_error = true;
    strcpy(token, setup_token);
    assert(provisioning_claim(token, "+1234", "example.apn", &admin) == ESP_FAIL);
    assert(!provisioning_owner() && !provisioning_setup_authorized(token));
    reboot();
    assert(provisioning_state() == PROVISION_CORRUPT);

    fresh_flash();
    commit_error = true;
    strcpy(token, setup_token);
    assert(provisioning_claim(token, "+1234", "example.apn", &admin) == ESP_FAIL);
    assert(!provisioning_owner() && !provisioning_setup_authorized(token));
    // A commit error can follow a durable write. Only a validated record
    // loaded on reboot may enable SMS or administrator authentication.
    reboot();
    assert(provisioning_state() == PROVISION_CONFIGURED);
    puts("Provisioning claims, legacy quarantine, errors and reboot persistence passed");
}
