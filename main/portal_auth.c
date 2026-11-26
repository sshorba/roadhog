#include "portal_auth.h"
#include "provisioning.h"
#include "owner_verification.h"
#include "setup_button.h"
#include "portal.h"
#include "sms_commands.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/platform_util.h"
#include "psa/crypto.h"

enum { SESSION_BYTES = 32 };
static const int64_t session_duration = 30LL * 60 * 1000000;

// HTTP handlers share one server task.
static char session[SESSION_BYTES * 2 + 1];
static int64_t session_expires;
static int64_t next_login_attempt;

esp_err_t portal_auth_init(void)
{
    return psa_crypto_init() == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
}

static bool derive_password(const char *password, const password_record_t *record, uint8_t *hash)
{
    psa_key_derivation_operation_t operation = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t status = psa_key_derivation_setup(&operation, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_integer(&operation, PSA_KEY_DERIVATION_INPUT_COST, 60000);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_SALT,
                                                record->salt, sizeof(record->salt));
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                                (const uint8_t *)password, strlen(password));
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_output_bytes(&operation, hash, sizeof(record->hash));
    }
    psa_key_derivation_abort(&operation);
    return status == PSA_SUCCESS;
}

static bool password_matches(const char *password)
{
    const password_record_t *credential = provisioning_admin();
    if (!credential) {
        return false;
    }
    uint8_t hash[sizeof(credential->hash)];
    bool matches = derive_password(password, credential, hash) &&
                   mbedtls_ct_memcmp(hash, credential->hash, sizeof(hash)) == 0;
    mbedtls_platform_zeroize(hash, sizeof(hash));
    return matches;
}

static bool prepare_password(const char *password, password_record_t *updated)
{
    esp_fill_random(updated->salt, sizeof(updated->salt));
    return derive_password(password, updated, updated->hash);
}

static bool save_password(const char *password)
{
    password_record_t updated;
    bool saved = prepare_password(password, &updated) &&
                 provisioning_save_admin(&updated) == ESP_OK;
    mbedtls_platform_zeroize(&updated, sizeof(updated));
    return saved;
}

bool portal_authenticated(httpd_req_t *request)
{
    char cookies[256];
    if (esp_timer_get_time() >= session_expires ||
        httpd_req_get_hdr_value_str(request, "Cookie", cookies, sizeof(cookies)) != ESP_OK) {
        return false;
    }
    char *remaining = cookies;
    for (char *cookie; (cookie = strsep(&remaining, ";")) != NULL;) {
        cookie += strspn(cookie, " ");
        const char prefix[] = "roadhog_session=";
        if (strncmp(cookie, prefix, sizeof(prefix) - 1) == 0) {
            const char *token = cookie + sizeof(prefix) - 1;
            return strlen(token) == sizeof(session) - 1 &&
                   mbedtls_ct_memcmp(token, session, sizeof(session) - 1) == 0;
        }
    }
    return false;
}

static esp_err_t auth_response(httpd_req_t *request, const char *status, const char *message)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(request, message);
}

static esp_err_t end_session(httpd_req_t *request)
{
    session_expires = 0;
    mbedtls_platform_zeroize(session, sizeof(session));
    httpd_resp_set_hdr(request, "Set-Cookie",
                       "roadhog_session=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict");
    return auth_response(request, "200 OK", "");
}

static esp_err_t login(httpd_req_t *request, const char *username, const char *password)
{
    int64_t now = esp_timer_get_time();
    if (now < next_login_attempt) {
        return auth_response(request, "429 Too Many Requests", "Wait a moment and try again.");
    }
    next_login_attempt = now + 1000000;
    if (strcmp(username, "admin") != 0 || !password_matches(password)) {
        return auth_response(request, "401 Unauthorized", "Incorrect username or password.");
    }

    uint8_t random[SESSION_BYTES];
    esp_fill_random(random, sizeof(random));
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(random); i++) {
        session[2 * i] = hex[random[i] >> 4];
        session[2 * i + 1] = hex[random[i] & 15];
    }
    session_expires = esp_timer_get_time() + session_duration;
    char cookie[128];
    snprintf(cookie, sizeof(cookie), "roadhog_session=%s; Path=/; HttpOnly; SameSite=Strict", session);
    httpd_resp_set_hdr(request, "Set-Cookie", cookie);
    return auth_response(request, "200 OK", "");
}

static esp_err_t change_password(httpd_req_t *request, const char *current, const char *updated)
{
    if (strlen(updated) < ADMIN_PASSWORD_MIN) {
        return auth_response(request, "400 Bad Request", "Use at least 12 bytes for the new password.");
    }
    if (!password_matches(current)) {
        return auth_response(request, "403 Forbidden", "Incorrect current password.");
    }
    if (strcmp(updated, current) == 0) {
        return auth_response(request, "400 Bad Request", "Choose a different password.");
    }
    if (!save_password(updated)) {
        return auth_response(request, "500 Internal Server Error", "Could not save the password.");
    }
    return end_session(request);
}

static bool receive_body(httpd_req_t *request, char *body, size_t length);

static const char *users_http_status(const char *message)
{
    if (!strcmp(message, "Sign in again.")) {
        return "401 Unauthorized";
    }
    if (!strcmp(message, "SMS users are unavailable.")) {
        return "503 Service Unavailable";
    }
    if (!strcmp(message, "Enter add or remove and a phone number.") ||
        !strcmp(message, "Use a number with + and country code.")) {
        return "400 Bad Request";
    }
    if (!strcmp(message, "Could not save user list")) {
        return "500 Internal Server Error";
    }
    if (!strcmp(message, "User added") || !strcmp(message, "User removed")) {
        return "200 OK";
    }
    return "409 Conflict";
}

static esp_err_t users_post(httpd_req_t *request)
{
    // The administrator session is the only portal role. Do not log the number.
    if (!portal_authenticated(request)) {
        return auth_response(request, "401 Unauthorized", "Sign in again.");
    }
    sms_control_t *sms = portal_bound_sms();
    if (!sms || !sms->owner || !sms->users_store) {
        return auth_response(request, "503 Service Unavailable", "SMS users are unavailable.");
    }
    char body[64];
    size_t length = request->content_len;
    if (!length || length >= sizeof(body) || !receive_body(request, body, length)) {
        mbedtls_platform_zeroize(body, sizeof(body));
        return auth_response(request, "400 Bad Request", "Enter add or remove and a phone number.");
    }
    char *number = strchr(body, '\n');
    esp_err_t result;
    if (!number || strchr(number + 1, '\n')) {
        result = auth_response(request, "400 Bad Request", "Enter add or remove and a phone number.");
    } else {
        *number++ = '\0';
        const char *message = sms_users_request(sms, true, body, number);
        result = auth_response(request, users_http_status(message), message);
    }
    mbedtls_platform_zeroize(body, sizeof(body));
    return result;
}

static bool receive_body(httpd_req_t *request, char *body, size_t length)
{
    for (size_t offset = 0; offset < length;) {
        int received = httpd_req_recv(request, body + offset, length - offset);
        if (received <= 0) {
            return false;
        }
        offset += received;
    }
    body[length] = '\0';
    return !memchr(body, '\0', length) && !strchr(body, '\r');
}

static esp_err_t verification_action(httpd_req_t *request, const char *action, const char *body)
{
    if (!strcmp(action, "send") && body && !strchr(body, '\n')) {
        setup_button_state_t button = setup_button_state();
        if (button == SETUP_BUTTON_WAITING || button == SETUP_BUTTON_CONFIRMED) {
            return auth_response(request, "409 Conflict", "Cancel button confirmation before requesting another code.");
        }
        esp_err_t error = owner_verification_request(body);
        if (error == ESP_OK) {
            return auth_response(request, "202 Accepted", "Waiting for the modem to send the code.");
        }
        if (error == ESP_ERR_INVALID_ARG) {
            return auth_response(request, "400 Bad Request", "Enter a valid E.164 owner number.");
        }
        return auth_response(request, "429 Too Many Requests",
                             "Wait at least a minute between requests. Up to five sends per 15 minutes.");
    }
    if (!strcmp(action, "cancel") && !body) {
        provisioning_cancel_claim();
        return auth_response(request, "200 OK", "Verification canceled.");
    }
    if (!strcmp(action, "status") && !body) {
        static const char *const labels[] = {
            [OWNER_VERIFY_IDLE] = "Request a code for the owner number.",
            [OWNER_VERIFY_QUEUED] = "Waiting for LTE registration and SMS service.",
            [OWNER_VERIFY_SENDING] = "Sending the verification SMS.",
            [OWNER_VERIFY_SENT] = "The modem accepted the SMS. Enter the code when it arrives.",
            [OWNER_VERIFY_FAILED] = "SMS send failed. Check service and retry. If the modem is offline, restart both devices.",
            [OWNER_VERIFY_EXPIRED] = "The request expired. Request a new code when SMS service is available.",
            [OWNER_VERIFY_LOCKED] = "Too many guesses. Request a new code.",
        };
        return auth_response(request, "200 OK", labels[owner_verification_status()]);
    }
    return auth_response(request, "400 Bad Request", "Invalid verification request.");
}

static esp_err_t finish_setup(httpd_req_t *request, const char *token)
{
    const char *wifi_password;
    switch (provisioning_finish_claim(token, &wifi_password)) {
    case ESP_OK:
        return auth_response(request, "200 OK", wifi_password);
    case ESP_ERR_NOT_FINISHED:
        return auth_response(request, "202 Accepted", "Waiting for the external setup button.");
    case ESP_ERR_TIMEOUT:
        return auth_response(request, "408 Request Timeout", "Button window expired. Request a new SMS code.");
    case ESP_ERR_NOT_ALLOWED:
        return auth_response(request, "409 Conflict", "No setup confirmation is pending.");
    case ESP_ERR_INVALID_STATE:
        return auth_response(request, "403 Forbidden", "Setup session unavailable. Restart setup if needed.");
    default:
        return auth_response(request, "500 Internal Server Error", "Setup could not be saved. Restart the device before retrying.");
    }
}

static esp_err_t setup_post(httpd_req_t *request, bool show_sim)
{
    char body[SETUP_SECRET_LENGTH + OWNER_CAPACITY + APN_CAPACITY + ADMIN_PASSWORD_MAX +
              OWNER_CODE_LENGTH + sizeof("confirm") + 5];
    size_t length = request->content_len;
    esp_err_t result;
    if (!length || length >= sizeof(body) || !receive_body(request, body, length)) {
        result = auth_response(request, "400 Bad Request", "Invalid setup request.");
        goto done;
    }
    if (!show_sim && !strcmp(body, "session")) {
        const char *session_token = provisioning_start_session();
        result = session_token ? auth_response(request, "201 Created", session_token) :
                 auth_response(request, "409 Conflict", "A setup session is already active, or the device is configured.");
        goto done;
    }
    char *rest = body;
    const char *token = strsep(&rest, "\n");
    if (show_sim) {
        result = !rest && provisioning_setup_completed(token) ? portal_sim_response(request) :
                 auth_response(request, "403 Forbidden", "Complete setup to view the SIM phone number.");
        goto done;
    }
    const char *action = strsep(&rest, "\n");
    if (action && !strcmp(action, "finish") && !rest) {
        result = finish_setup(request, token);
        goto done;
    }
    if (!action || !provisioning_setup_authorized(token)) {
        result = auth_response(request, "403 Forbidden", "Start or resume setup in this browser.");
        goto done;
    }
    if (strcmp(action, "confirm")) {
        result = verification_action(request, action, rest);
        goto done;
    }
    const char *owner = strsep(&rest, "\n");
    const char *code = strsep(&rest, "\n");
    const char *apn = strsep(&rest, "\n");
    if (!owner || !code || !apn || !rest || strchr(rest, '\n') ||
        strlen(rest) < ADMIN_PASSWORD_MIN || strlen(rest) > ADMIN_PASSWORD_MAX) {
        result = auth_response(request, "400 Bad Request", "Check the setup fields and use a 12–64 byte password.");
        goto done;
    }
    password_record_t updated;
    esp_err_t error = prepare_password(rest, &updated)
                      ? provisioning_claim(token, owner, apn, code, &updated) : ESP_FAIL;
    mbedtls_platform_zeroize(&updated, sizeof(updated));
    if (error == ESP_OK) {
        result = auth_response(request, "202 Accepted", "SMS verified. Press the external setup button within 30 seconds.");
    } else if (error == ESP_ERR_NOT_ALLOWED) {
        result = auth_response(request, "403 Forbidden", "Code not verified. Check the number and code, wait a second before retrying, or request a new code.");
    } else if (error == ESP_ERR_INVALID_STATE) {
        result = auth_response(request, "409 Conflict", "A button confirmation is already pending, or setup has ended.");
    } else if (error == ESP_ERR_INVALID_ARG) {
        result = auth_response(request, "400 Bad Request", "Enter a valid E.164 owner number and carrier APN.");
    } else {
        result = auth_response(request, "500 Internal Server Error", "Setup could not be verified. Restart the device before retrying.");
    }
done:
    mbedtls_platform_zeroize(body, sizeof(body));
    return result;
}

esp_err_t portal_auth_post(httpd_req_t *request)
{
    if (!strcmp(request->uri, "/api/setup") || !strcmp(request->uri, "/api/setup/sim")) {
        return setup_post(request, !strcmp(request->uri, "/api/setup/sim"));
    }
    bool signing_in = strcmp(request->uri, "/api/login") == 0;
    if (!signing_in && !portal_authenticated(request)) {
        return auth_response(request, "401 Unauthorized", "Sign in again.");
    }
    if (strcmp(request->uri, "/api/logout") == 0) {
        return end_session(request);
    }
    if (strcmp(request->uri, "/api/users") == 0) {
        return users_post(request);
    }

    // Two UTF-8 fields separated by a newline. Password inputs are single-line.
    char body[2 * ADMIN_PASSWORD_MAX + 2];
    size_t length = request->content_len;
    if (length == 0 || length >= sizeof(body)) {
        return auth_response(request, "400 Bad Request", "Enter values of up to 64 bytes each.");
    }
    if (!receive_body(request, body, length)) {
        mbedtls_platform_zeroize(body, sizeof(body));
        return auth_response(request, "400 Bad Request", "Invalid credentials format.");
    }

    char *second = strchr(body, '\n');
    esp_err_t result;
    if (!second || strchr(second + 1, '\n') ||
        (size_t)(second - body) > ADMIN_PASSWORD_MAX || strlen(second + 1) > ADMIN_PASSWORD_MAX) {
        result = auth_response(request, "400 Bad Request", "Invalid credentials format.");
    } else {
        *second++ = '\0';
        result = signing_in ? login(request, body, second) : change_password(request, body, second);
    }
    mbedtls_platform_zeroize(body, sizeof(body));
    return result;
}
