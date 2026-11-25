#include "sms_commands.h"
#include "provisioning.h"
#include "portal.h"
#include "connection_status.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *const unknown = "Unknown command, send \"?\" for help.";

// The HTTP task and the modem task both read and write the additional-user list.
static SemaphoreHandle_t users_mu;
#ifdef CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION
static StaticSemaphore_t users_mu_storage;
#endif

// False means the list must not be touched. A missing lock is that failure.
static bool users_lock(void)
{
    if (!users_mu) {
        return false;
    }
    return xSemaphoreTake(users_mu, portMAX_DELAY) == pdTRUE;
}

static void users_unlock(void)
{
    xSemaphoreGive(users_mu);
}

void sms_users_lock_init(void)
{
    if (users_mu) {
        return;
    }
#ifdef CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION
    users_mu = xSemaphoreCreateMutexStatic(&users_mu_storage);
#else
    users_mu = xSemaphoreCreateMutex();
#endif
}

bool sms_number_valid(const char *number)
{
    return provisioning_owner_valid(number);
}

bool sms_sender_allowed(const sms_control_t *sms, const char *sender)
{
    if (!sms->owner || !sms_number_valid(sender)) {
        return false;
    }
    if (strcmp(sms->owner, sender) == 0) {
        return true;
    }
    if (!users_lock()) {
        return false;
    }
    bool allowed = false;
    for (size_t i = 0; i < SMS_USER_LIMIT; i++) {
        if (strcmp(sms->users[i], sender) == 0) {
            allowed = true;
        }
    }
    users_unlock();
    return allowed;
}

bool sms_users_load(sms_control_t *sms)
{
    if (nvs_flash_init() != ESP_OK ||
        nvs_open("sms", NVS_READWRITE, &sms->users_store) != ESP_OK) {
        return false;
    }
    size_t length = sizeof(sms->users);
    esp_err_t error = nvs_get_blob(sms->users_store, "users_v1", sms->users, &length);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (error != ESP_OK || length != sizeof(sms->users)) {
        goto invalid;
    }
    for (size_t i = 0; i < SMS_USER_LIMIT; i++) {
        if (!memchr(sms->users[i], '\0', SMS_NUMBER_CAPACITY) ||
            (sms->users[i][0] && !sms_number_valid(sms->users[i]))) {
            goto invalid;
        }
    }
    return true;

invalid:
    memset(sms->users, 0, sizeof(sms->users));
    nvs_close(sms->users_store);
    sms->users_store = 0;
    return false;
}

static const char *apply_user_change(sms_control_t *sms, const char *number, bool add)
{
    if (!sms_number_valid(number)) {
        return "Use a number with + and country code.";
    }
    if (!sms->owner || strcmp(number, sms->owner) == 0) {
        return "Owner is permanent";
    }

    char updated[SMS_USER_LIMIT][SMS_NUMBER_CAPACITY];
    memcpy(updated, sms->users, sizeof(updated));
    bool existing = false;
    char *empty = NULL;
    for (size_t i = 0; i < SMS_USER_LIMIT; i++) {
        if (strcmp(updated[i], number) == 0) {
            existing = true;
            if (!add) {
                // Revoke every stored entry, including duplicates from older data.
                memset(updated[i], 0, SMS_NUMBER_CAPACITY);
            }
        } else if (!updated[i][0]) {
            empty = updated[i];
        }
    }
    if (add) {
        if (existing) {
            return "User already authorized";
        }
        if (!empty) {
            return "User list full";
        }
        strcpy(empty, number);
    } else if (!existing) {
        return "User not found";
    }
    if (!sms->users_store) {
        return "SMS users are unavailable.";
    }
    if (nvs_set_blob(sms->users_store, "users_v1", updated, sizeof(updated)) != ESP_OK ||
        nvs_commit(sms->users_store) != ESP_OK) {
        return "Could not save user list";
    }
    memcpy(sms->users, updated, sizeof(updated));
    return add ? "User added" : "User removed";
}

static const char *sms_users_change(sms_control_t *sms, const char *number, bool add)
{
    if (!sms || !number) {
        return "SMS users are unavailable.";
    }
    if (!users_lock()) {
        return "SMS users are unavailable.";
    }
    const char *message = apply_user_change(sms, number, add);
    users_unlock();
    return message;
}

const char *sms_users_request(sms_control_t *sms, bool authenticated, const char *action,
                              const char *number)
{
    if (!authenticated) {
        return "Sign in again.";
    }
    if (!sms || !sms->owner || !sms->users_store) {
        return "SMS users are unavailable.";
    }
    bool add;
    if (action && strcmp(action, "add") == 0) {
        add = true;
    } else if (action && strcmp(action, "remove") == 0) {
        add = false;
    } else {
        return "Enter add or remove and a phone number.";
    }
    if (!number) {
        return "Enter add or remove and a phone number.";
    }
    return sms_users_change(sms, number, add);
}

static bool sms_users_copy(const sms_control_t *sms, char copied[SMS_USER_LIMIT][SMS_NUMBER_CAPACITY])
{
    if (!users_lock()) {
        memset(copied, 0, sizeof(sms->users));
        return false;
    }
    memcpy(copied, sms->users, sizeof(sms->users));
    users_unlock();
    return true;
}

bool sms_users_json(const sms_control_t *sms, char *json, size_t capacity)
{
    if (!sms || !sms->owner || !sms_number_valid(sms->owner) || !json) {
        return false;
    }
    char copied[SMS_USER_LIMIT][SMS_NUMBER_CAPACITY];
    if (!sms_users_copy(sms, copied)) {
        return false;
    }
    int used = snprintf(json, capacity, "{\"owner\":\"%s\",\"users\":[", sms->owner);
    if (used < 0 || (size_t)used >= capacity) {
        return false;
    }
    bool first = true;
    for (size_t i = 0; i < SMS_USER_LIMIT; i++) {
        if (!copied[i][0]) {
            continue;
        }
        if (!sms_number_valid(copied[i])) {
            return false;
        }
        int wrote = snprintf(json + used, capacity - (size_t)used,
                             "%s{\"number\":\"%s\",\"role\":\"authorized\"}",
                             first ? "" : ",", copied[i]);
        if (wrote < 0 || (size_t)wrote >= capacity - (size_t)used) {
            return false;
        }
        used += wrote;
        first = false;
    }
    int end = snprintf(json + used, capacity - (size_t)used, "]}");
    return end > 0 && (size_t)end < capacity - (size_t)used;
}

static const char *change_user(sms_control_t *sms, const char *sender,
                               const char *number, bool add)
{
    if (strcmp(sender, sms->owner) != 0) {
        return "Owner only";
    }
    return sms_users_change(sms, number, add);
}

static void gps_reply(modem_t *modem, bool map, char reply[SMS_TEXT_CAPACITY])
{
    gps_fix_t fix;
    gps_status_t status = read_gps(modem, &fix);
    if (status != GPS_FIX) {
        if (map) {
            strcpy(reply, "No GPS");
        } else {
            snprintf(reply, SMS_TEXT_CAPACITY,
                     "GPS: %s\nSpeed: unavailable\nCoordinates: unavailable", gps_label(status));
        }
        return;
    }
    if (map) {
        snprintf(reply, SMS_TEXT_CAPACITY, "https://www.google.com/maps/search/?api=1&query=%.6f,%.6f",
                 fix.latitude, fix.longitude);
        return;
    }
    char speed[40] = "unavailable";
    if (isfinite(fix.speed_knots)) {
        snprintf(speed, sizeof(speed), "%.1f km/h", fix.speed_knots * 1.852);
    }
    snprintf(reply, SMS_TEXT_CAPACITY, "GPS: Fix\nSpeed: %s\nCoordinates: %.6f,%.6f",
             speed, fix.latitude, fix.longitude);
}

static void status_reply(modem_t *modem, char reply[SMS_TEXT_CAPACITY])
{
    gps_fix_t fix;
    const char *gps = gps_label(read_gps(modem, &fix));
    const char *registration = lte_label(read_lte_registration(modem));
    const char *data = lte_data_context(modem);
    snprintf(reply, SMS_TEXT_CAPACITY, "ROAD HOG ONLINE\nGPS: %s\nLTE: %s\nData context: %s",
             gps, registration, data);
}

void sms_command_reply(sms_control_t *sms, modem_t *modem, const sms_request_t *request,
                       char reply[SMS_TEXT_CAPACITY])
{
    const char *command = request->text;
    const char *message = unknown;
    if (strcmp(command, "?") == 0) {
        message = "Commands:\n? - Help\nSTATUS - Online\nGPS - Fix, speed, coordinates\nMAP - Google Maps\nUSERS - Authorized numbers\nADDUSER +number\nDELUSER +number\nAPON / APOFF";
    } else if (strcmp(command, "APON") == 0 || strcmp(command, "APOFF") == 0) {
        bool enable = strcmp(command, "APON") == 0;
        if (!sms_sender_allowed(sms, request->sender)) {
            message = "Unauthorized";
        } else if (!enable && strcmp(request->sender, sms->owner) != 0) {
            message = "Owner only";
        } else {
            esp_err_t error = portal_request(enable);
            if (error == ESP_OK) {
                message = enable ? "AP enabled" : "AP disabled";
                if (enable && strcmp(request->sender, sms->owner) == 0 &&
                    provisioning_state() == PROVISION_CONFIGURED) {
                    const char *password = provisioning_wifi_password();
                    char ssid[PORTAL_SSID_CAPACITY];
                    if (password && portal_get_ssid(ssid) == ESP_OK) {
                        snprintf(reply, SMS_TEXT_CAPACITY, "AP enabled\nSSID: %s\nWi-Fi password: %s",
                                 ssid, password);
                        return;
                    }
                }
            } else if (error == ESP_ERR_INVALID_STATE && !enable) {
                message = "AP stays enabled: cellular unavailable";
            } else {
                message = "Could not change AP state";
            }
        }
    } else if (strcmp(command, "STATUS") == 0) {
        status_reply(modem, reply);
        return;
    } else if (strcmp(command, "GPS") == 0 || strcmp(command, "MAP") == 0) {
        gps_reply(modem, command[0] == 'M', reply);
        return;
    } else if (strcmp(command, "USERS") == 0) {
        char listed[SMS_USER_LIMIT][SMS_NUMBER_CAPACITY];
        if (!sms_users_copy(sms, listed)) {
            strcpy(reply, "Could not read the user list");
            return;
        }
        snprintf(reply, SMS_TEXT_CAPACITY, "Authorized numbers:\n%s (owner)", sms->owner);
        for (size_t i = 0; i < SMS_USER_LIMIT; i++) {
            if (listed[i][0]) {
                size_t used = strlen(reply);
                snprintf(reply + used, SMS_TEXT_CAPACITY - used, "\n%s", listed[i]);
            }
        }
        return;
    } else if (strncmp(command, "ADDUSER ", 8) == 0 || strncmp(command, "DELUSER ", 8) == 0) {
        message = change_user(sms, request->sender, command + 8, command[0] == 'A');
    }
    strcpy(reply, message);
}
