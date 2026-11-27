#include "ota_sms.h"

#include <string.h>

ota_sms_command_t ota_sms_parse(const char *text)
{
    if (strcmp(text, "OTA CHECK") == 0) {
        return OTA_SMS_CHECK;
    }
    if (strcmp(text, "OTA CANCEL") == 0) {
        return OTA_SMS_CANCEL;
    }
    if (strcmp(text, "OTA STATUS") == 0) {
        return OTA_SMS_STATUS;
    }
    if (strncmp(text, "OTA CONFIRM ", 12) == 0 &&
        strlen(text + 12) == OTA_SMS_CODE_LENGTH &&
        strspn(text + 12, "0123456789ABCDEF") == OTA_SMS_CODE_LENGTH) {
        return OTA_SMS_CONFIRM;
    }
    return (strcmp(text, "OTA") == 0 || strncmp(text, "OTA ", 4) == 0)
           ? OTA_SMS_INVALID : OTA_SMS_NONE;
}

void ota_sms_issue(ota_sms_challenge_t *challenge, const ota_manifest_t *release,
                   const uint8_t random[6], int64_t now)
{
    static const char hex[] = "0123456789ABCDEF";
    challenge->release = *release;
    for (size_t i = 0; i < 6; ++i) {
        challenge->code[2 * i] = hex[random[i] >> 4];
        challenge->code[2 * i + 1] = hex[random[i] & 15];
    }
    challenge->code[OTA_SMS_CODE_LENGTH] = 0;
    challenge->expires = now + OTA_SMS_LIFETIME_US;
}

bool ota_sms_pending(ota_sms_challenge_t *challenge, int64_t now)
{
    if (now >= challenge->expires) {
        *challenge = (ota_sms_challenge_t){0};
    }
    return challenge->expires != 0;
}

bool ota_sms_confirm(ota_sms_challenge_t *challenge, const char *code,
                     int64_t now, ota_manifest_t *release)
{
    if (!ota_sms_pending(challenge, now) || strlen(code) != OTA_SMS_CODE_LENGTH) {
        return false;
    }
    unsigned difference = 0;
    for (size_t i = 0; i < OTA_SMS_CODE_LENGTH; ++i) {
        difference |= (unsigned char)challenge->code[i] ^ (unsigned char)code[i];
    }
    if (difference != 0) {
        return false;
    }
    *release = challenge->release;
    *challenge = (ota_sms_challenge_t){0};
    return true;
}

bool ota_sms_same_release(const ota_manifest_t *a, const ota_manifest_t *b)
{
    return a->sequence == b->sequence && a->size == b->size &&
           memcmp(a->digest, b->digest, sizeof(a->digest)) == 0;
}
