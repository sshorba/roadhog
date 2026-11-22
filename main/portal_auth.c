#include "portal_auth.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "psa/crypto.h"

enum { PASSWORD_MIN = 8, PASSWORD_MAX = 64, SESSION_BYTES = 32 };
static const char *const nvs_namespace = "portal_auth";
static const char *const credential_key = "password_v1";
static const char default_password[] = "roadhog";
static const int64_t session_duration = 30LL * 60 * 1000000;

typedef struct {
    uint8_t salt[16];
    uint8_t hash[32];
} password_record_t;

// HTTP handlers share one server task. Initialization finishes before it starts.
static password_record_t credential;
static bool password_changed;
static char session[SESSION_BYTES * 2 + 1];
static int64_t session_expires;
static int64_t next_login_attempt;

esp_err_t portal_auth_init(void)
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(nvs_namespace, NVS_READONLY, &nvs);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    size_t length = sizeof(credential);
    error = nvs_get_blob(nvs, credential_key, &credential, &length);
    nvs_close(nvs);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error == ESP_OK && length != sizeof(credential)) {
        return ESP_ERR_INVALID_SIZE;
    }
    password_changed = error == ESP_OK;
    return error;
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
    if (!password_changed) {
        return strcmp(password, default_password) == 0;
    }
    uint8_t hash[sizeof(credential.hash)];
    bool matches = derive_password(password, &credential, hash) &&
                   mbedtls_ct_memcmp(hash, credential.hash, sizeof(hash)) == 0;
    mbedtls_platform_zeroize(hash, sizeof(hash));
    return matches;
}

static bool save_password(const char *password)
{
    password_record_t updated;
    esp_fill_random(updated.salt, sizeof(updated.salt));
    if (!derive_password(password, &updated, updated.hash)) {
        return false;
    }
    nvs_handle_t nvs;
    if (nvs_open(nvs_namespace, NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t error = nvs_set_blob(nvs, credential_key, &updated, sizeof(updated));
    if (error == ESP_OK) {
        error = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (error != ESP_OK) {
        return false;
    }
    credential = updated;
    password_changed = true;
    return true;
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
    if (strlen(updated) < PASSWORD_MIN) {
        return auth_response(request, "400 Bad Request", "Use at least 8 bytes for the new password.");
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

esp_err_t portal_auth_post(httpd_req_t *request)
{
    bool signing_in = strcmp(request->uri, "/api/login") == 0;
    if (!signing_in && !portal_authenticated(request)) {
        return auth_response(request, "401 Unauthorized", "Sign in again.");
    }
    if (strcmp(request->uri, "/api/logout") == 0) {
        return end_session(request);
    }

    // Two UTF-8 fields separated by a newline. Password inputs are single-line.
    char body[2 * PASSWORD_MAX + 2];
    size_t length = request->content_len;
    if (length == 0 || length >= sizeof(body)) {
        return auth_response(request, "400 Bad Request", "Enter values of up to 64 bytes each.");
    }
    for (size_t offset = 0; offset < length;) {
        int received = httpd_req_recv(request, body + offset, length - offset);
        if (received <= 0) {
            mbedtls_platform_zeroize(body, sizeof(body));
            return ESP_FAIL;
        }
        offset += received;
    }

    body[length] = '\0';
    char *second = strchr(body, '\n');
    esp_err_t result;
    if (!second || memchr(body, '\0', length) || strchr(body, '\r') || strchr(second + 1, '\n') ||
        (size_t)(second - body) > PASSWORD_MAX || strlen(second + 1) > PASSWORD_MAX) {
        result = auth_response(request, "400 Bad Request", "Invalid credentials format.");
    } else {
        *second++ = '\0';
        result = signing_in ? login(request, body, second) : change_password(request, body, second);
    }
    mbedtls_platform_zeroize(body, sizeof(body));
    return result;
}
