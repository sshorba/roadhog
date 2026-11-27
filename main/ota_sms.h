#pragma once

#include "ota_manifest.h"

#define OTA_SMS_CODE_LENGTH 12
#define OTA_SMS_LIFETIME_US INT64_C(300000000)

typedef enum {
    OTA_SMS_NONE,
    OTA_SMS_INVALID,
    OTA_SMS_CHECK,
    OTA_SMS_CONFIRM,
    OTA_SMS_CANCEL,
    OTA_SMS_STATUS,
} ota_sms_command_t;

typedef struct {
    ota_manifest_t release;
    char code[OTA_SMS_CODE_LENGTH + 1];
    int64_t expires;
} ota_sms_challenge_t;

ota_sms_command_t ota_sms_parse(const char *text);
void ota_sms_issue(ota_sms_challenge_t *challenge, const ota_manifest_t *release,
                   const uint8_t random[6], int64_t now);
bool ota_sms_pending(ota_sms_challenge_t *challenge, int64_t now);
bool ota_sms_confirm(ota_sms_challenge_t *challenge, const char *code,
                     int64_t now, ota_manifest_t *release);
bool ota_sms_same_release(const ota_manifest_t *a, const ota_manifest_t *b);
