#include "sms_commands.h"
#include "provisioning.h"
#include "portal.h"
#include "connection_status.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nvs_flash.h"
#include "nvs.h"

static const char *const unknown = "Unknown command, send \"?\" for help.";

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
    for (size_t i = 0; i < SMS_USER_LIMIT; i++) {
        if (strcmp(sms->users[i], sender) == 0) {
            return true;
        }
    }
    return false;
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
    nvs_close(sms->users_store);
    return false;
}

static const char *change_user(sms_control_t *sms, const char *sender,
                               const char *number, bool add)
{
    if (strcmp(sender, sms->owner) != 0) {
        return "Owner only";
    }
    if (!sms_number_valid(number)) {
        return "Use a number with + and country code.";
    }
    if (strcmp(number, sms->owner) == 0) {
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
    if (nvs_set_blob(sms->users_store, "users_v1", updated, sizeof(updated)) != ESP_OK ||
        nvs_commit(sms->users_store) != ESP_OK) {
        return "Could not save user list";
    }
    memcpy(sms->users, updated, sizeof(updated));
    return add ? "User added" : "User removed";
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
        if (strcmp(request->sender, sms->owner) != 0) {
            message = "Owner only";
        } else {
            bool enable = strcmp(command, "APON") == 0;
            esp_err_t error = portal_request(enable);
            if (error == ESP_OK) {
                message = enable ? "AP enabled" : "AP disabled";
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
        snprintf(reply, SMS_TEXT_CAPACITY, "Authorized numbers:\n%s (owner)", sms->owner);
        for (size_t i = 0; i < SMS_USER_LIMIT; i++) {
            if (sms->users[i][0]) {
                size_t used = strlen(reply);
                snprintf(reply + used, SMS_TEXT_CAPACITY - used, "\n%s", sms->users[i]);
            }
        }
        return;
    } else if (strncmp(command, "ADDUSER ", 8) == 0 || strncmp(command, "DELUSER ", 8) == 0) {
        message = change_user(sms, request->sender, command + 8, command[0] == 'A');
    }
    strcpy(reply, message);
}
