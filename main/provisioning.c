#include "provisioning.h"
#include "owner_verification.h"
#include "setup_button.h"

#include <stddef.h>
#include <string.h>

#include "esp_timer.h"
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
static int64_t session_expires;
static bool claim_locked, claim_saved;
static config_record_t pending;
static const int64_t session_duration = INT64_C(15) * 60 * 1000000;
static const char factory_wifi_password[] = "012345678";

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
        if (setup_button_init() != ESP_OK) {
            return;
        }
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
    if (state == PROVISION_FACTORY) {
        return factory_wifi_password;
    }
    return state == PROVISION_CONFIGURED ? record.wifi_password : NULL;
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

static bool session_matches(const char *token)
{
    return state == PROVISION_FACTORY && esp_timer_get_time() < session_expires &&
           strnlen(token, sizeof(setup_token)) == SETUP_SECRET_LENGTH &&
           mbedtls_ct_memcmp(token, setup_token, SETUP_SECRET_LENGTH) == 0;
}

const char *provisioning_start_session(void)
{
    if (state != PROVISION_FACTORY || claim_locked || esp_timer_get_time() < session_expires) {
        return NULL;
    }
    provisioning_cancel_claim();
    generate_secret(setup_token); // The AP is running, so RF entropy is available.
    session_expires = esp_timer_get_time() + session_duration;
    return setup_token;
}

bool provisioning_setup_authorized(const char *token)
{
    return !claim_locked && session_matches(token);
}

void provisioning_cancel_claim(void)
{
    setup_button_cancel();
    owner_verification_cancel();
    mbedtls_platform_zeroize(&pending, sizeof(pending));
}

bool provisioning_setup_completed(const char *token)
{
    return claim_saved && session_matches(token);
}

esp_err_t provisioning_claim(const char *token, const char *owner, const char *apn,
                             const char *code, const password_record_t *admin)
{
    if (!provisioning_setup_authorized(token) ||
        setup_button_state() == SETUP_BUTTON_WAITING ||
        setup_button_state() == SETUP_BUTTON_CONFIRMED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!provisioning_owner_valid(owner) || !apn_valid(apn) || !admin_valid(admin)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!owner_verification_consume(owner, code)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    pending = (config_record_t){.version = 1, .admin = *admin};
    strcpy(pending.owner, owner);
    strcpy(pending.apn, apn);
    generate_secret(pending.wifi_password);
    // Give the authorized browser the entire confirmation window.
    session_expires = esp_timer_get_time() + session_duration;
    setup_button_arm();
    return ESP_OK;
}

esp_err_t provisioning_finish_claim(const char *token, const char **wifi_password)
{
    if (provisioning_setup_completed(token)) {
        *wifi_password = record.wifi_password;
        return ESP_OK;
    }
    if (!provisioning_setup_authorized(token)) {
        return ESP_ERR_INVALID_STATE;
    }
    switch (setup_button_state()) {
    case SETUP_BUTTON_WAITING:
        return ESP_ERR_NOT_FINISHED;
    case SETUP_BUTTON_CONFIRMED:
        break;
    case SETUP_BUTTON_EXPIRED:
        provisioning_cancel_claim();
        return ESP_ERR_TIMEOUT;
    default:
        return ESP_ERR_NOT_ALLOWED;
    }

    // Repeat polls can retrieve the saved password but cannot repeat a write.
    claim_locked = true;
    esp_err_t error = commit(&pending);
    if (error == ESP_OK) {
        memcpy(record.wifi_password, pending.wifi_password, sizeof(record.wifi_password));
        claim_saved = true;
        session_expires = esp_timer_get_time() + session_duration;
        *wifi_password = record.wifi_password;
    }
    provisioning_cancel_claim();
    return error;
}
