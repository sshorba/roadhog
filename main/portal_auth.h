#pragma once

#include <stdbool.h>

#include "esp_http_server.h"

esp_err_t portal_auth_init(void);
bool portal_authenticated(httpd_req_t *request);
esp_err_t portal_auth_post(httpd_req_t *request);
