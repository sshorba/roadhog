#pragma once

#include "modem.h"
#include "sms_control.h"
#include "sdkconfig.h"

#ifdef CONFIG_ROADHOG_OTA_UPDATE_BENCH
modem_t *ota_update_poll(modem_t *modem, const modem_config_t *config, sms_control_t *sms);
#endif

#ifdef CONFIG_ROADHOG_OTA_SMS_BENCH
void ota_sms_init(void);
void ota_sms_ack_failed(void);
bool ota_sms_service_command(const sms_control_t *sms, modem_t *modem, const sms_request_t *request);
#endif
