#pragma once

#include "modem.h"
#include "sms_control.h"
#include "sdkconfig.h"

#ifdef CONFIG_ROADHOG_OTA_UPDATE_BENCH
modem_t *ota_update_poll(modem_t *modem, const modem_config_t *config, sms_control_t *sms);
#endif
