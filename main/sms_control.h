#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "modem.h"
#include "nvs.h"

enum { SMS_NUMBER_CAPACITY = 17, SMS_TEXT_CAPACITY = 161, SMS_USER_LIMIT = 6,
       SMS_QUEUE_CAPACITY = 4 };

typedef struct {
    char sender[SMS_NUMBER_CAPACITY];
    char text[SMS_TEXT_CAPACITY];
} sms_request_t;

typedef struct sms_control {
    const char *owner;
    char users[SMS_USER_LIMIT][SMS_NUMBER_CAPACITY];
    nvs_handle_t users_store;
    sms_request_t requests[SMS_QUEUE_CAPACITY];
    unsigned request_head;
    unsigned request_count;
    unsigned pending_acks;
    size_t expected_tpdu;
    bool configured;
} sms_control_t;

void sms_control_disconnect(sms_control_t *sms);
void sms_control_start(sms_control_t *sms, modem_t *modem);
void sms_control_resume(sms_control_t *sms, modem_t *modem);
bool sms_control_notification(sms_control_t *sms, const char *line);
void sms_control_service(sms_control_t *sms, modem_t *modem);

void sms_owner_verification_service(modem_t *modem);
