#include "connection_status.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "connection_status";

gps_session_t gps_session_state(modem_t *modem)
{
    char response[64];
    modem_result_t result = {0};
    esp_err_t error = modem_command(modem, "AT+CGPS?", "+CGPS:", 3000,
                                    response, sizeof(response), &result);
    if (error != ESP_OK || result.final != MODEM_FINAL_OK ||
        strncmp(response, "+CGPS:", 6) != 0) {
        return GPS_SESSION_UNKNOWN;
    }
    const char *state = response + 6;
    state += strspn(state, " \t");
    if ((state[0] != '0' && state[0] != '1') || (state[1] != ',' && state[1] != '\n')) {
        return GPS_SESSION_UNKNOWN;
    }
    return state[0] == '1' ? GPS_SESSION_RUNNING : GPS_SESSION_STOPPED;
}

bool start_gps(modem_t *modem)
{
    gps_session_t state = gps_session_state(modem);
    if (state == GPS_SESSION_RUNNING) {
        ESP_LOGI(TAG, "GPS session already running");
        return true;
    }
    if (state != GPS_SESSION_STOPPED) {
        return false;
    }
    modem_result_t result = {0};
    esp_err_t error = modem_command(modem, "AT+CGPS=1", NULL, 5000, NULL, 0, &result);
    if (error == ESP_OK && result.final == MODEM_FINAL_OK) {
        ESP_LOGI(TAG, "GPS session started");
        return true;
    }
    ESP_LOGW(TAG, "GPS start failed: %s",
             error == ESP_OK ? result.final_line : esp_err_to_name(error));
    return false;
}

static bool read_decimal(const char *text, double *value)
{
    if (!text || !*text) {
        return false;
    }
    char *end;
    *value = strtod(text, &end);
    return !*end && isfinite(*value) && *value >= 0;
}

static bool coordinate(const char *raw, const char *direction, char positive,
                       char negative, int limit, double *value)
{
    double degrees_minutes;
    if (!read_decimal(raw, &degrees_minutes) || degrees_minutes > limit * 100.0 ||
        !direction || !direction[0] || direction[1] ||
        (direction[0] != positive && direction[0] != negative)) {
        return false;
    }
    int degrees = (int)(degrees_minutes / 100);
    double minutes = degrees_minutes - degrees * 100;
    if (minutes >= 60) {
        return false;
    }
    *value = (degrees + minutes / 60) * (direction[0] == negative ? -1 : 1);
    return true;
}

static bool query(modem_t *modem, const char *command, const char *prefix,
                  char *response, size_t capacity)
{
    modem_result_t result;
    return modem_command(modem, command, prefix, 3000, response, capacity, &result) == ESP_OK &&
           result.final == MODEM_FINAL_OK;
}

gps_status_t read_gps(modem_t *modem, gps_fix_t *fix)
{
    char response[MODEM_LINE_CAPACITY];
    if (!query(modem, "AT+CGPSINFO", "+CGPSINFO:", response, sizeof(response)) || !response[0]) {
        return GPS_UNAVAILABLE;
    }

    char *data = response + strlen("+CGPSINFO:");
    char *fields[9];
    for (size_t i = 0; i < 9; i++) {
        fields[i] = strsep(&data, ",\n");
    }
    if (!coordinate(fields[0], fields[1], 'N', 'S', 90, &fix->latitude) ||
        !coordinate(fields[2], fields[3], 'E', 'W', 180, &fix->longitude)) {
        return GPS_NO_FIX;
    }
    if (!read_decimal(fields[7], &fix->speed_knots) || fix->speed_knots >= 10000) {
        fix->speed_knots = NAN;
    }
    return GPS_FIX;
}

const char *gps_label(gps_status_t status)
{
    switch (status) {
    case GPS_FIX:
        return "Fix";
    case GPS_NO_FIX:
        return "No fix";
    default:
        return "Unavailable";
    }
}

lte_status_t read_lte_registration(modem_t *modem)
{
    char response[MODEM_LINE_CAPACITY];
    if (!query(modem, "AT+CEREG?", "+CEREG:", response, sizeof(response))) {
        return LTE_UNKNOWN;
    }
    char *remaining = response;
    for (char *line; (line = strsep(&remaining, "\n")) != NULL;) {
        unsigned mode, status;
        int end = 0;
        if (sscanf(line, "+CEREG: %u , %u %n", &mode, &status, &end) != 2 ||
            (line[end] && line[end] != ',')) {
            continue;
        }
        switch (status) {
        case 0: return LTE_NOT_REGISTERED;
        case 1: return LTE_HOME;
        case 2: return LTE_SEARCHING;
        case 3: return LTE_DENIED;
        case 5: return LTE_ROAMING;
        default: return LTE_UNKNOWN;
        }
    }
    return LTE_UNKNOWN;
}

const char *lte_label(lte_status_t status)
{
    switch (status) {
    case LTE_NOT_REGISTERED: return "Not registered";
    case LTE_HOME: return "Registered (home)";
    case LTE_SEARCHING: return "Searching";
    case LTE_DENIED: return "Registration denied";
    case LTE_ROAMING: return "Registered (roaming)";
    default: return "Unknown";
    }
}

const char *lte_data_context(modem_t *modem)
{
    char response[MODEM_LINE_CAPACITY];
    if (!query(modem, "AT+CGACT?", "+CGACT:", response, sizeof(response))) {
        return "Unknown";
    }
    bool malformed = false;
    char *remaining = response;
    for (char *line; (line = strsep(&remaining, "\n")) != NULL;) {
        if (!*line || strcmp(line, "+CGACT:") == 0 || strcmp(line, "+CGACT: ") == 0) {
            continue;
        }
        unsigned context, active;
        int end = 0;
        if (sscanf(line, "+CGACT: %u , %u %n", &context, &active, &end) != 2 ||
            line[end] || context == 0 || active > 1) {
            malformed = true;
        } else if (active == 1) {
            return "Active";
        }
    }
    return malformed ? "Unknown" : "Inactive";
}
