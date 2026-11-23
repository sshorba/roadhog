#include "provisioning.h"
#include "owner_verification.h"

#include <stddef.h>
#include <string.h>

#include "bootloader_random.h"
#include "esp_random.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char config_namespace[] = "device_cfg";
static const char config_key[] = "settings_v1";

typedef struct {
    uint8_t version;
    char owner[OWNER_CAPACITY];
    char apn[APN_CAPACITY];
    char wifi_password[SETUP_SECRET_LENGTH + 1];
    password_record_t admin;
} config_record_t;

// Boot state and network/owner values are immutable until reboot. Only the
// HTTP server task commits claims and changes the cached administrator hash.
static provisioning_state_t state;
static config_record_t record;
static char setup_token[SETUP_SECRET_LENGTH + 1];
static bool claim_locked;

bool provisioning_owner_valid(const char *owner)
{
    size_t length = strnlen(owner, OWNER_CAPACITY);
    return length >= 3 && length < OWNER_CAPACITY && owner[0] == '+' &&
           owner[1] >= '1' && owner[1] <= '9' && strspn(owner + 1, "0123456789") == length - 1;
}

static bool apn_valid(const char *apn)
{
    size_t length = strnlen(apn, APN_CAPACITY);
    return length && length < APN_CAPACITY &&
           strspn(apn, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") == length;
}

static bool admin_valid(const password_record_t *admin)
{
    const uint8_t zero[32] = {0};
    return memcmp(admin->hash, zero, sizeof(admin->hash)) &&
           memcmp(admin->salt, zero, sizeof(admin->salt));
}

static bool record_valid(const config_record_t *config)
{
    static const char hex[] = "0123456789abcdef";
    return config->version == 1 && provisioning_owner_valid(config->owner) &&
           apn_valid(config->apn) && config->wifi_password[SETUP_SECRET_LENGTH] == '\0' &&
           strspn(config->wifi_password, hex) == SETUP_SECRET_LENGTH &&
           admin_valid(&config->admin);
}

static bool namespace_absent(const char *name)
{
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(name, NVS_READONLY, &nvs);
    if (error == ESP_OK) {
        nvs_close(nvs);
    }
    return error == ESP_ERR_NVS_NOT_FOUND;
}

static void generate_secret(char secret[SETUP_SECRET_LENGTH + 1])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t random[SETUP_SECRET_LENGTH / 2];
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); ++i) {
        secret[2 * i] = hex[random[i] >> 4];
        secret[2 * i + 1] = hex[random[i] & 15];
    }
    secret[SETUP_SECRET_LENGTH] = '\0';
    mbedtls_platform_zeroize(random, sizeof(random));
}

void provisioning_init(void)
{
    if (nvs_flash_init() != ESP_OK) {
        return;
    }
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(config_namespace, NVS_READONLY, &nvs);
    if (error == ESP_OK) {
        size_t length = sizeof(record);
        error = nvs_get_blob(nvs, config_key, &record, &length);
        nvs_close(nvs);
        if (error == ESP_OK && length == sizeof(record) && record_valid(&record)) {
            state = PROVISION_CONFIGURED;
        }
    } else if (error == ESP_ERR_NVS_NOT_FOUND &&
               namespace_absent("sms") && namespace_absent("portal_auth")) {
        // Called before Wi-Fi, ADC use, or tasks serving the portal.
        bootloader_random_enable();
        generate_secret(record.wifi_password);
        generate_secret(setup_token);
        bootloader_random_disable();
        state = PROVISION_FACTORY;
    }
}

provisioning_state_t provisioning_state(void) { return state; }

const char *provisioning_owner(void)
{
    return state == PROVISION_CONFIGURED ? record.owner : NULL;
}

const char *provisioning_apn(void)
{
    return state == PROVISION_CONFIGURED ? record.apn : NULL;
}

const char *provisioning_wifi_password(void)
{
    return state == PROVISION_CORRUPT ? NULL : record.wifi_password;
}

const password_record_t *provisioning_admin(void)
{
    return state == PROVISION_CONFIGURED ? &record.admin : NULL;
}

static esp_err_t commit(const config_record_t *updated)
{
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(config_namespace, NVS_READWRITE, &nvs);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_blob(nvs, config_key, updated, sizeof(*updated));
    if (error == ESP_OK) {
        error = nvs_commit(nvs);
    }
    if (error == ESP_OK) {
        config_record_t readback;
        size_t length = sizeof(readback);
        error = nvs_get_blob(nvs, config_key, &readback, &length);
        if (error == ESP_OK && (length != sizeof(readback) ||
                                memcmp(&readback, updated, sizeof(readback)))) {
            error = ESP_FAIL;
        }
        mbedtls_platform_zeroize(&readback, sizeof(readback));
    }
    nvs_close(nvs);
    return error;
}

esp_err_t provisioning_save_admin(const password_record_t *admin)
{
    if (state != PROVISION_CONFIGURED || !admin_valid(admin)) {
        return ESP_ERR_INVALID_STATE;
    }
    config_record_t updated = record;
    updated.admin = *admin;
    esp_err_t error = commit(&updated);
    if (error == ESP_OK) {
        record.admin = *admin;
    }
    mbedtls_platform_zeroize(&updated, sizeof(updated));
    return error;
}

bool provisioning_setup_authorized(const char *token)
{
    // The draft has no production interface that can disclose this secret.
    // Its per-boot credential handoff requires separate physical review.
    return state == PROVISION_FACTORY && !claim_locked &&
           strnlen(token, sizeof(setup_token)) == SETUP_SECRET_LENGTH &&
           mbedtls_ct_memcmp(token, setup_token, SETUP_SECRET_LENGTH) == 0;
}

esp_err_t provisioning_claim(const char *token, const char *owner, const char *apn,
                             const char *code, const password_record_t *admin)
{
    if (!provisioning_setup_authorized(token)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!provisioning_owner_valid(owner) || !apn_valid(apn) || !admin_valid(admin)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!owner_verification_consume(owner, code)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    config_record_t updated = {.version = 1, .admin = *admin};
    strcpy(updated.owner, owner);
    strcpy(updated.apn, apn);
    memcpy(updated.wifi_password, record.wifi_password, sizeof(updated.wifi_password));

    // Lock out repeat claims even after an uncertain storage result.
    claim_locked = true;
    mbedtls_platform_zeroize(setup_token, sizeof(setup_token));
    esp_err_t error = commit(&updated);
    mbedtls_platform_zeroize(&updated, sizeof(updated));
    return error;
}
