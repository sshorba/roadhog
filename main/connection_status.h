#pragma once

#include <stdbool.h>

#include "modem.h"

typedef enum {
    GPS_UNAVAILABLE,
    GPS_NO_FIX,
    GPS_FIX,
} gps_status_t;

typedef struct {
    double latitude;
    double longitude;
    double speed_knots;
} gps_fix_t;

typedef enum {
    LTE_UNKNOWN,
    LTE_NOT_REGISTERED,
    LTE_HOME,
    LTE_SEARCHING,
    LTE_DENIED,
    LTE_ROAMING,
} lte_status_t;

bool gps_session_running(modem_t *modem);
bool start_gps(modem_t *modem);
gps_status_t read_gps(modem_t *modem, gps_fix_t *fix);
const char *gps_label(gps_status_t status);
lte_status_t read_lte_registration(modem_t *modem);
const char *lte_label(lte_status_t status);
const char *lte_data_context(modem_t *modem);
