#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "provisioning.h"
#include "owner_verification.h"
#include "setup_button.h"

typedef unsigned nvs_handle_t;
enum { NVS_READONLY, NVS_READWRITE };
static uint8_t stored[512];
static size_t stored_length;
static uint8_t users_stored[102];
static size_t users_length;
static bool users_set_error, users_commit_error;
static bool config_exists, legacy_sms, legacy_admin;
static bool init_error, read_error, set_error, commit_error;
static unsigned writes;
static uint8_t entropy;

static esp_err_t nvs_flash_init(void) { return init_error ? ESP_FAIL : ESP_OK; }

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
    if (!strcmp(name, "sms") && mode == NVS_READWRITE) {
        *handle = 3;
        return ESP_OK;
    }
    assert(mode == NVS_READONLY);
    if ((!strcmp(name, "sms") && legacy_sms) || (!strcmp(name, "portal_auth") && legacy_admin)) {
        *handle = 2;
        return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

static void nvs_close(nvs_handle_t handle) { assert(handle == 1 || handle == 2 || handle == 3); }

static esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *dst, size_t *length)
{
    if (handle == 3) {
        assert(!strcmp(key, "users_v1"));
        if (!users_length) {
            return ESP_ERR_NVS_NOT_FOUND;
        }
        if (*length >= users_length) {
            memcpy(dst, users_stored, users_length);
        }
        *length = users_length;
        return ESP_OK;
    }
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
    if (handle == 3) {
        assert(!strcmp(key, "users_v1") && length == sizeof(users_stored));
        ++writes;
        if (users_set_error) {
            return ESP_FAIL;
        }
        memcpy(users_stored, input, length);
        users_length = length;
        return ESP_OK;
    }
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
    if (handle == 3) {
        return users_commit_error ? ESP_FAIL : ESP_OK;
    }
    assert(handle == 1);
    return commit_error ? ESP_FAIL : ESP_OK;
}

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
static int64_t clock_us;
static uint32_t random_value;
static int64_t esp_timer_get_time(void) { return clock_us; }
static uint32_t esp_random(void) { return ++random_value; }

enum { GPIO_NUM_4 = 4, GPIO_MODE_INPUT = 1, GPIO_PULLUP_ENABLE = 1,
       GPIO_PULLDOWN_DISABLE = 0, GPIO_INTR_DISABLE = 0 };
#define CONFIG_ROADHOG_STATUS_LED_GPIO 38
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
typedef struct {
    void (*callback)(void *);
    const char *name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;
typedef unsigned esp_timer_handle_t;
static bool pin_high;
static unsigned button_inits;

static esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(config->pin_bit_mask == (1U << GPIO_NUM_4) && config->mode == GPIO_MODE_INPUT);
    assert(config->pull_up_en == GPIO_PULLUP_ENABLE && config->pull_down_en == GPIO_PULLDOWN_DISABLE);
    assert(config->intr_type == GPIO_INTR_DISABLE);
    ++button_inits;
    return ESP_OK;
}

static int gpio_get_level(int pin) { assert(pin == GPIO_NUM_4); return pin_high; }

static esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer)
{
    assert(args->callback && args->name && args->skip_unhandled_events);
    *timer = 1;
    return ESP_OK;
}

static esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t interval)
{
    assert(timer == 1 && interval == 10000);
    return ESP_OK;
}

static void esp_timer_delete(esp_timer_handle_t timer) { assert(timer == 1); }

#include "setup_button_under_test.inc"

static void sample(bool high, int64_t elapsed)
{
    clock_us += elapsed;
    pin_high = high;
    sample_button(NULL);
}

static void press_button(void)
{
    sample(true, SAMPLE_US);
    sample(true, DEBOUNCE_US);
    sample(false, SAMPLE_US);
    sample(false, DEBOUNCE_US);
    assert(setup_button_state() == SETUP_BUTTON_CONFIRMED);
}

#include "owner_verification_under_test.inc"
#include "provisioning_under_test.inc"

typedef struct { bool reject_command, reject_sms; unsigned attempts; } modem_t;
typedef struct { int final; } modem_result_t;
enum { MODEM_FINAL_OK = 1 };

static esp_err_t modem_command(modem_t *modem, const char *command, const char *prefix,
                               unsigned timeout, char *response, size_t capacity,
                               modem_result_t *result)
{
    assert(!strcmp(command, "ATE0") || !strcmp(command, "AT+CMGF=0"));
    assert(!prefix && timeout == 5000 && !response && !capacity);
    result->final = MODEM_FINAL_OK;
    return modem->reject_command ? ESP_FAIL : ESP_OK;
}

static esp_err_t modem_submit_sms(modem_t *modem, unsigned length, const char *text,
                                  modem_result_t *result)
{
    (void)result;
    ++modem->attempts;
    uint8_t pdu[160];
    assert(length + 1 <= sizeof(pdu) && strlen(text) == (length + 1) * 2);
    for (size_t i = 0; i <= length; ++i) {
        unsigned byte;
        assert(sscanf(text + 2 * i, "%2x", &byte) == 1);
        pdu[i] = byte;
    }
    assert(pdu[0] == 0 && pdu[3] == 4 && pdu[4] == 0x91);
    assert(pdu[5] == 0x21 && pdu[6] == 0x43); // +1234
    char decoded[48] = {0};
    assert(pdu[9] < sizeof(decoded));
    for (size_t i = 0; i < pdu[9]; ++i) {
        size_t bit = i * 7;
        unsigned value = pdu[10 + bit / 8] >> (bit % 8);
        if (bit % 8 > 1) {
            value |= pdu[11 + bit / 8] << (8 - bit % 8);
        }
        decoded[i] = value & 0x7f;
    }
    char expected[48];
    snprintf(expected, sizeof(expected), "Road Hog owner verification code: %s", challenge.code);
    assert(!strcmp(decoded, expected));
    return modem->reject_sms ? ESP_FAIL : ESP_OK;
}

typedef esp_err_t (*modem_sms_claim_fn)(void *);
static esp_err_t modem_submit_sms_claimed(modem_t *modem, unsigned length, const char *text,
                                          modem_result_t *result, modem_sms_claim_fn claim,
                                          void *context)
{
    assert(claim == NULL && context == NULL);
    return modem_submit_sms(modem, length, text, result);
}

enum { SMS_TEXT_CAPACITY = 161 };
bool sms_number_valid(const char *number);
#include "sms_delivery_under_test.inc"

static owner_verify_message_t deliver(const char *owner, bool accepted)
{
    owner_verify_message_t sent;
    assert(owner_verification_request(owner) == ESP_OK);
    assert(owner_verification_take(&sent));
    assert(!strcmp(owner, sent.owner));
    owner_verification_finish(accepted);
    return sent;
}

enum { SMS_NUMBER_CAPACITY = OWNER_CAPACITY, SMS_USER_LIMIT = 6 };
typedef struct {
    const char *owner;
    char users[SMS_USER_LIMIT][SMS_NUMBER_CAPACITY];
    nvs_handle_t users_store;
} sms_control_t;

static bool users_lock_ok = true;
static bool users_lock(void) { return users_lock_ok; }
static void users_unlock(void) {}

#include "sms_authorization_under_test.inc"

enum { PORTAL_SSID_CAPACITY = sizeof("ROADHOG-FFFF") };
typedef struct { char sender[SMS_NUMBER_CAPACITY], text[SMS_TEXT_CAPACITY]; } sms_request_t;
static const char *const unknown = "Unknown command";
static bool ap_running;
static unsigned ap_starts;

static esp_err_t portal_request(bool enabled)
{
    if (enabled && !ap_running) {
        ++ap_starts;
    }
    ap_running = enabled;
    return ESP_OK;
}

static esp_err_t portal_get_ssid(char ssid[PORTAL_SSID_CAPACITY])
{
    strcpy(ssid, "ROADHOG-ABCD");
    return ESP_OK;
}

static void status_reply(modem_t *modem, char reply[SMS_TEXT_CAPACITY])
{
    (void)modem;
    strcpy(reply, "ROAD HOG ONLINE");
}

static void gps_reply(modem_t *modem, bool map, char reply[SMS_TEXT_CAPACITY])
{
    (void)modem;
    (void)map;
    strcpy(reply, "GPS unavailable");
}

#include "sms_commands_under_test.inc"

static void test_ap_credentials(sms_control_t *sms, const char *password)
{
    unsigned previous_writes = writes;
    uint8_t previous_entropy = entropy;
    char reply[SMS_TEXT_CAPACITY];
    sms_request_t request = {.sender = "+9876", .text = "APON"};
    sms_command_reply(sms, NULL, &request, reply);
    assert(!strcmp(reply, "Unauthorized") && !ap_running && !strstr(reply, password));

    strcpy(request.sender, "+5678");
    sms_command_reply(sms, NULL, &request, reply);
    assert(ap_running && ap_starts == 1 && !strcmp(reply, "AP enabled"));
    assert(!strstr(reply, password));
    strcpy(request.text, "APOFF");
    sms_command_reply(sms, NULL, &request, reply);
    assert(ap_running && !strcmp(reply, "Owner only"));

    strcpy(request.sender, "+1234");
    strcpy(request.text, "APON");
    sms_command_reply(sms, NULL, &request, reply);
    assert(ap_running && ap_starts == 1);
    assert(strstr(reply, "SSID: ROADHOG-ABCD") && strstr(reply, password));
    strcpy(request.text, "APOFF");
    sms_command_reply(sms, NULL, &request, reply);
    assert(!ap_running && !strstr(reply, password));
    strcpy(request.text, "APON");
    sms_command_reply(sms, NULL, &request, reply);
    assert(ap_running && ap_starts == 2 && strstr(reply, password));

    const char *unrelated[] = {"STATUS", "?", "GPS", "MAP", "USERS", "UNKNOWN"};
    for (size_t i = 0; i < sizeof(unrelated) / sizeof(*unrelated); ++i) {
        strcpy(request.text, unrelated[i]);
        sms_command_reply(sms, NULL, &request, reply);
        assert(!strstr(reply, password));
    }
    assert(writes == previous_writes && entropy == previous_entropy);
    assert(!strcmp(provisioning_wifi_password(), password));
}

static void reboot(void)
{
    state = PROVISION_CORRUPT;
    claim_locked = claim_saved = false;
    session_expires = 0;
    memset(&pending, 0, sizeof(pending));
    setup_button_cancel();
    memset(&record, 0, sizeof(record));
    memset(setup_token, 0, sizeof(setup_token));
    clear(OWNER_VERIFY_IDLE);
    next_send = next_guess = deadline = clock_us = 0;
    send_count = guesses = 0;
    in_flight = false;
    provisioning_init();
}

static void fresh_flash(void)
{
    config_exists = legacy_sms = legacy_admin = false;
    init_error = read_error = set_error = commit_error = false;
    stored_length = 0;
    users_length = 0;
    users_set_error = users_commit_error = false;
    memset(users_stored, 0, sizeof(users_stored));
    writes = 0;
    reboot();
}

static void test_portal_sms_users(void)
{
    assert(SMS_USER_LIMIT * SMS_NUMBER_CAPACITY == sizeof(users_stored));
    users_length = 0;
    users_set_error = users_commit_error = false;
    sms_control_t sms = {.owner = "+1234"};
    assert(sms_users_load(&sms) && sms.users_store);
    unsigned before = writes;
    assert(!strcmp(sms_users_request(&sms, false, "add", "+1999"), "Sign in again."));
    assert(writes == before && !sms_sender_allowed(&sms, "+1999"));
    assert(!strcmp(sms_users_request(&sms, true, "promote", "+1999"),
                   "Enter add or remove and a phone number."));
    assert(!strcmp(sms_users_request(&sms, true, "add", "1999"),
                   "Use a number with + and country code."));
    assert(!strcmp(sms_users_request(&sms, true, "add", "+1234"), "Owner is permanent"));
    assert(!strcmp(sms_users_request(&sms, true, "remove", "+1234"), "Owner is permanent"));
    assert(writes == before && sms_sender_allowed(&sms, "+1234"));

    assert(!strcmp(sms_users_request(&sms, true, "add", "+1999"), "User added"));
    assert(sms_sender_allowed(&sms, "+1999"));
    assert(!strcmp(sms_users_request(&sms, true, "add", "+1999"), "User already authorized"));
    char json[512];
    assert(sms_users_json(&sms, json, sizeof(json)));
    assert(strstr(json, "\"owner\":\"+1234\"") && strstr(json, "\"number\":\"+1999\""));
    assert(strstr(json, "\"role\":\"authorized\"") && !strstr(json, "+1234\",\"role\""));

    sms_control_t loaded = {.owner = "+1234"};
    assert(sms_users_load(&loaded));
    assert(sms_sender_allowed(&loaded, "+1999"));
    assert(!strcmp(sms_users_request(&loaded, true, "remove", "+1888"), "User not found"));
    assert(!strcmp(sms_users_request(&loaded, true, "remove", "+1999"), "User removed"));
    assert(!sms_sender_allowed(&loaded, "+1999"));
    sms_control_t after_reboot = {.owner = "+1234"};
    assert(sms_users_load(&after_reboot));
    assert(!after_reboot.users[0][0] && !sms_sender_allowed(&after_reboot, "+1999"));

    users_set_error = true;
    before = writes;
    assert(!strcmp(sms_users_request(&after_reboot, true, "add", "+1777"), "Could not save user list"));
    assert(!sms_sender_allowed(&after_reboot, "+1777") && writes == before + 1);
    users_set_error = false;
    users_commit_error = true;
    assert(!strcmp(sms_users_request(&after_reboot, true, "add", "+1777"), "Could not save user list"));
    assert(!sms_sender_allowed(&after_reboot, "+1777"));
    users_commit_error = false;
    assert(!strcmp(sms_users_request(&after_reboot, true, "add", "+1777"), "User added"));

    for (int i = 0; i < SMS_USER_LIMIT - 1; i++) {
        char number[8];
        snprintf(number, sizeof(number), "+100%d", i);
        assert(!strcmp(sms_users_request(&after_reboot, true, "add", number), "User added"));
    }
    assert(!strcmp(sms_users_request(&after_reboot, true, "add", "+1009"), "User list full"));
    assert(!sms_sender_allowed(&after_reboot, "+1009"));
    users_lock_ok = false;
    before = writes;
    assert(!strcmp(sms_users_request(&after_reboot, true, "add", "+1666"),
                   "SMS users are unavailable."));
    assert(writes == before);
    assert(sms_sender_allowed(&after_reboot, "+1234"));
    assert(!sms_sender_allowed(&after_reboot, "+1777"));
    assert(!sms_users_json(&after_reboot, json, sizeof(json)));
    users_lock_ok = true;
    assert(sms_sender_allowed(&after_reboot, "+1777"));
    assert(sms_users_json(&after_reboot, json, sizeof(json)));
    assert(!sms_users_json(&after_reboot, json, 32));

    after_reboot.users[2][0] = '\0';
    strcpy(after_reboot.users[4], "+1777");
    assert(!strcmp(sms_users_request(&after_reboot, true, "remove", "+1777"), "User removed"));
    assert(!sms_sender_allowed(&after_reboot, "+1777"));
    for (size_t i = 0; i < SMS_USER_LIMIT; i++) {
        assert(strcmp(after_reboot.users[i], "+1777"));
    }

    users_length = 4;
    users_stored[0] = 'x';
    sms_control_t corrupt = {.owner = "+1234"};
    assert(!sms_users_load(&corrupt) && !corrupt.users_store && !corrupt.users[0][0]);
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
    assert(!strcmp(provisioning_wifi_password(), "012345678"));
    assert(!provisioning_setup_authorized(""));
    strcpy(token, provisioning_start_session());
    assert(!provisioning_start_session());
    assert(provisioning_claim("012345678", "+1234", "example.apn", "", &admin) == ESP_ERR_INVALID_STATE);
    assert(provisioning_claim(token, "1234", "example.apn", "", &admin) == ESP_ERR_INVALID_ARG);
    assert(provisioning_claim(token, "+1234", "x\rAT", "", &admin) == ESP_ERR_INVALID_ARG);
    assert(!writes);
    reboot();
    assert(provisioning_state() == PROVISION_FACTORY && !provisioning_setup_authorized(token));
    strcpy(token, provisioning_start_session());
    assert(provisioning_claim(token, "+1234", "example.apn", "000000", &admin) == ESP_ERR_NOT_ALLOWED);
    owner_verify_message_t sent = deliver("+1234", true);
    assert(provisioning_claim(token, "+5678", "example.apn", sent.code, &admin) == ESP_ERR_NOT_ALLOWED);
    assert(!writes && !provisioning_owner());
    assert(provisioning_claim(token, "+1234", "example.apn", sent.code, &admin) == ESP_ERR_NOT_ALLOWED);
    clock_us += SECOND;
    assert(provisioning_claim(token, "+1234", "example.apn", sent.code, &admin) == ESP_OK);
    assert(!provisioning_owner());
    assert(provisioning_claim(token, "+5678", "example.apn", "", &admin) == ESP_ERR_INVALID_STATE);
    const char *saved_wifi;
    assert(provisioning_finish_claim(token, &saved_wifi) == ESP_ERR_NOT_FINISHED);
    assert(!writes);
    // A button held at window start cannot count. Release must be stable.
    sample(false, SAMPLE_US);
    sample(false, DEBOUNCE_US);
    assert(setup_button_state() == SETUP_BUTTON_WAITING);
    sample(true, SAMPLE_US);
    sample(false, SAMPLE_US);
    sample(false, DEBOUNCE_US);
    assert(setup_button_state() == SETUP_BUTTON_WAITING);
    // After a stable release, a brief low glitch still cannot confirm.
    sample(true, SAMPLE_US);
    sample(true, DEBOUNCE_US);
    sample(false, SAMPLE_US);
    sample(true, SAMPLE_US);
    assert(setup_button_state() == SETUP_BUTTON_WAITING);
    press_button();
    assert(!writes);
    assert(provisioning_finish_claim("another-session", &saved_wifi) == ESP_ERR_INVALID_STATE);
    assert(!writes);
    assert(provisioning_finish_claim(token, &saved_wifi) == ESP_OK);
    assert(strlen(saved_wifi) == SETUP_SECRET_LENGTH && strcmp(saved_wifi, "012345678"));
    strcpy(ap_key, saved_wifi);
    assert(provisioning_finish_claim(token, &saved_wifi) == ESP_OK);
    assert(provisioning_setup_completed(token) && writes == 1);
    unsigned previous_inits = button_inits;
    reboot();
    assert(button_inits == previous_inits && !provisioning_start_session());
    sample(false, 2 * DEBOUNCE_US);
    sample(true, 2 * DEBOUNCE_US);
    assert(writes == 1 && setup_button_state() == SETUP_BUTTON_IDLE);
    assert(provisioning_state() == PROVISION_CONFIGURED);
    assert(!strcmp(provisioning_owner(), "+1234") && !strcmp(provisioning_apn(), "example.apn"));
    sms.owner = provisioning_owner();
    assert(sms_sender_allowed(&sms, "+1234") && sms_sender_allowed(&sms, "+5678"));
    assert(!sms_sender_allowed(&sms, "+9876") && !sms_sender_allowed(&sms, ""));
    assert(!strcmp(change_user(&sms, "+5678", "+9876", true), "Owner only"));
    assert(!strcmp(change_user(&sms, "+1234", "+1234", false), "Owner is permanent"));
    assert(!strcmp(provisioning_wifi_password(), ap_key));
    test_ap_credentials(&sms, ap_key);
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
    strcpy(token, provisioning_start_session());
    sent = deliver("+1234", true);
    assert(provisioning_claim(token, "+1234", "example.apn", sent.code, &admin) == ESP_OK);
    press_button();
    assert(provisioning_finish_claim(token, &saved_wifi) == ESP_FAIL);
    assert(!provisioning_owner() && !provisioning_setup_authorized(token));
    reboot();
    assert(provisioning_state() == PROVISION_CORRUPT);

    fresh_flash();
    commit_error = true;
    strcpy(token, provisioning_start_session());
    sent = deliver("+1234", true);
    assert(provisioning_claim(token, "+1234", "example.apn", sent.code, &admin) == ESP_OK);
    press_button();
    assert(provisioning_finish_claim(token, &saved_wifi) == ESP_FAIL);
    assert(!provisioning_owner() && !provisioning_setup_authorized(token));
    // A commit error can follow a durable write. Only a validated record
    // loaded on reboot may enable SMS or administrator authentication.
    reboot();
    assert(provisioning_state() == PROVISION_CONFIGURED);
    fresh_flash();
    strcpy(token, provisioning_start_session());
    // A complete press before arming is ignored.
    sample(true, SAMPLE_US);
    sample(false, DEBOUNCE_US);
    sample(true, DEBOUNCE_US);
    sent = deliver("+1234", true);
    assert(provisioning_claim(token, "+1234", "example.apn", sent.code, &admin) == ESP_OK);
    sample(true, SAMPLE_US);
    sample(true, DEBOUNCE_US);
    clock_us = expires - 1;
    sample(false, 0);
    sample(false, DEBOUNCE_US);
    assert(provisioning_finish_claim(token, &saved_wifi) == ESP_ERR_TIMEOUT && !writes);
    clock_us += SEND_INTERVAL;
    sent = deliver("+1234", true);
    assert(provisioning_claim(token, "+1234", "example.apn", sent.code, &admin) == ESP_OK);
    press_button();
    provisioning_cancel_claim();
    assert(provisioning_finish_claim(token, &saved_wifi) == ESP_ERR_NOT_ALLOWED && !writes);
    clock_us += SEND_INTERVAL;
    sent = deliver("+1234", true);
    assert(provisioning_claim(token, "+1234", "example.apn", sent.code, &admin) == ESP_OK);
    reboot();
    assert(provisioning_finish_claim(token, &saved_wifi) == ESP_ERR_INVALID_STATE && !writes);
    strcpy(token, provisioning_start_session());
    clock_us = session_expires;
    assert(provisioning_start_session() && !provisioning_setup_authorized(token));

    fresh_flash();
    sent = deliver("+1234", false);
    assert(owner_verification_status() == OWNER_VERIFY_FAILED);
    assert(!owner_verification_consume("+1234", sent.code));
    assert(owner_verification_request("+5678") == ESP_ERR_TIMEOUT);
    clock_us += SEND_INTERVAL;
    sent = deliver("+1234", true);
    for (unsigned i = 0; i < GUESS_LIMIT; ++i) {
        assert(!owner_verification_consume("+1234", "999999"));
        clock_us += SECOND;
    }
    assert(owner_verification_status() == OWNER_VERIFY_LOCKED);
    assert(!owner_verification_consume("+1234", sent.code));
    clock_us = 2 * SEND_INTERVAL;
    sent = deliver("+1234", true);
    clock_us += CODE_LIFETIME;
    assert(!owner_verification_consume("+1234", sent.code));
    assert(owner_verification_status() == OWNER_VERIFY_EXPIRED);
    sent = deliver("+1234", true);
    clock_us += SEND_INTERVAL;
    owner_verify_message_t replacement = deliver("+1234", true);
    assert(strcmp(sent.code, replacement.code));
    assert(!owner_verification_consume("+1234", sent.code));
    clock_us += SECOND;
    assert(owner_verification_consume("+1234", replacement.code));
    assert(!owner_verification_consume("+1234", replacement.code));
    clock_us += SEND_INTERVAL;
    assert(owner_verification_request("+1234") == ESP_ERR_TIMEOUT);
    clock_us = SEND_WINDOW;
    assert(owner_verification_request("+1234") == ESP_OK);
    assert(owner_verification_take(&sent));
    owner_verification_cancel();
    assert(owner_verification_status() == OWNER_VERIFY_IDLE);
    owner_verification_finish(true);
    assert(!owner_verification_consume("+1234", sent.code));
    reboot();
    assert(!owner_verification_consume("+1234", sent.code) && !provisioning_owner());
    assert(owner_verification_request("+1234") == ESP_OK);
    clock_us += QUEUE_LIFETIME;
    assert(!owner_verification_take(&sent));
    assert(owner_verification_status() == OWNER_VERIFY_EXPIRED);
    fresh_flash();
    modem_t modem = {.reject_command = true};
    assert(owner_verification_request("+1234") == ESP_OK);
    sms_owner_verification_service(&modem);
    assert(owner_verification_status() == OWNER_VERIFY_FAILED && modem.attempts == 0);
    clock_us += SEND_INTERVAL;
    modem.reject_command = false;
    modem.reject_sms = true;
    assert(owner_verification_request("+1234") == ESP_OK);
    sms_owner_verification_service(&modem);
    assert(owner_verification_status() == OWNER_VERIFY_FAILED && modem.attempts == 1);
    clock_us += SEND_INTERVAL;
    modem.reject_sms = false;
    assert(owner_verification_request("+1234") == ESP_OK);
    sms_owner_verification_service(&modem);
    assert(owner_verification_status() == OWNER_VERIFY_SENT && modem.attempts == 2);
    assert(!provisioning_owner() && !writes);
    test_portal_sms_users();
    puts("Provisioning, GPIO4 debounce and session binding, SMS challenge and reboot tests passed");
}
