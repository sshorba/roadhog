#pragma once

#include <stdbool.h>

#include "connection_status.h"
#include "esp_http_server.h"

esp_err_t portal_init(void);
// Data is a static label from lte_data_context(), or NULL if unknown.
void portal_set_status(lte_status_t lte, const char *data, gps_status_t gps);

// An offline fallback overrides requests to disable the AP.
esp_err_t portal_request(bool enabled);

enum { PORTAL_SSID_CAPACITY = sizeof("ROADHOG-FFFF") };
esp_err_t portal_get_ssid(char ssid[PORTAL_SSID_CAPACITY]);

void portal_set_sim_number(const char *number);
esp_err_t portal_sim_response(httpd_req_t *request);

struct sms_control;
void portal_bind_sms(struct sms_control *sms);
struct sms_control *portal_bound_sms(void);
