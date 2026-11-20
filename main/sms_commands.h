#pragma once

#include "sms_control.h"

bool sms_number_valid(const char *number);
bool sms_sender_allowed(const sms_control_t *sms, const char *sender);
bool sms_users_load(sms_control_t *sms);
void sms_command_reply(sms_control_t *sms, modem_t *modem, const sms_request_t *request,
                       char reply[SMS_TEXT_CAPACITY]);
