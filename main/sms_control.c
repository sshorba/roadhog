#include "sms_control.h"
#include "sms_commands.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

#ifdef CONFIG_ROADHOG_SMS_CONTROL
static const char *TAG = "sms";
static const char owner[] = CONFIG_ROADHOG_SMS_OWNER;
static const char hex[] = "0123456789ABCDEF";

enum { MAX_PDU_BYTES = 190 };

static bool command_ok(modem_t *modem, const char *command)
{
    modem_result_t result;
    return modem_command(modem, command, NULL, 5000, NULL, 0, &result) == ESP_OK &&
           result.final == MODEM_FINAL_OK;
}

static void start_gps(modem_t *modem)
{
    char response[64];
    modem_result_t result = {0};
    esp_err_t error = modem_command(modem, "AT+CGPS?", "+CGPS:", 3000,
                                    response, sizeof(response), &result);
    if (error == ESP_OK && result.final == MODEM_FINAL_OK &&
        strncmp(response, "+CGPS:", 6) == 0 &&
        response[strspn(response + 6, " ") + 6] == '1') {
        ESP_LOGI(TAG, "GPS session already running");
        return;
    }
    error = modem_command(modem, "AT+CGPS=1", NULL, 5000, NULL, 0, &result);
    if (error == ESP_OK && result.final == MODEM_FINAL_OK) {
        ESP_LOGI(TAG, "GPS session started");
    } else {
        ESP_LOGW(TAG, "GPS start failed: %s",
                 error == ESP_OK ? result.final_line : esp_err_to_name(error));
    }
}

static int nibble(char c)
{
    const char *digit = strchr(hex, c);
    return digit && c ? (int)(digit - hex) : -1;
}

static bool decode_hex(const char *line, uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        int high = nibble(line[2 * i]);
        int low = nibble(line[2 * i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        bytes[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static uint8_t get_septet(const uint8_t *data, size_t i)
{
    size_t bit = i * 7;
    uint16_t value = data[bit / 8];
    if (bit % 8 > 1) {
        value |= (uint16_t)data[bit / 8 + 1] << 8;
    }
    return (value >> (bit % 8)) & 0x7f;
}

static bool decode_request(const uint8_t *pdu, size_t length, size_t tpdu_length,
                           sms_request_t *request)
{
    size_t smsc_length = pdu[0] + 1u;
    if (smsc_length >= length || length - smsc_length != tpdu_length || tpdu_length < 13) {
        return false;
    }
    const uint8_t *tpdu = pdu + smsc_length;
    size_t digits = tpdu[1];
    size_t pid = 3 + (digits + 1) / 2;
    if (digits == 0 || digits > 15 || tpdu_length < pid + 10 ||
        (tpdu[0] & 0x43) != 0 || tpdu[2] != 0x91 || tpdu[pid] != 0 ||
        tpdu[pid + 1] != 0) {
        return false;
    }
    request->sender[0] = '+';
    for (size_t i = 0; i < digits; i++) {
        unsigned digit = (tpdu[3 + i / 2] >> (4 * (i % 2))) & 0xf;
        if (digit > 9) {
            return false;
        }
        request->sender[i + 1] = '0' + digit;
    }
    if (digits % 2 && (tpdu[pid - 1] >> 4) != 0xf) {
        return false;
    }
    request->sender[digits + 1] = '\0';

    size_t chars = tpdu[pid + 9];
    if (chars >= SMS_TEXT_CAPACITY || tpdu_length != pid + 10 + (chars * 7 + 7) / 8) {
        return false;
    }
    for (size_t i = 0; i < chars; i++) {
        unsigned value = get_septet(tpdu + pid + 10, i);
        if (value >= 'a' && value <= 'z') {
            value -= 'a' - 'A';
        }
        // Preserve the command alphabet and prevent embedded NULs.
        request->text[i] = value >= 32 && value <= 90 ? value : 0x7f;
    }
    while (chars && request->text[chars - 1] == ' ') {
        chars--;
    }
    request->text[chars] = '\0';
    return true;
}

static esp_err_t send_reply(modem_t *modem, const char *number, const char *message)
{
    const char *digits = number + 1;
    size_t digit_count = strlen(digits);
    size_t length = strlen(message);
    uint8_t pdu[160] = {0x00, 0x01, 0x00, digit_count, 0x91};
    size_t used = 5;
    for (size_t i = 0; i < digit_count; i += 2) {
        unsigned high = i + 1 < digit_count ? (unsigned)(digits[i + 1] - '0') : 0xf;
        pdu[used++] = (high << 4) | (digits[i] - '0');
    }
    pdu[used++] = 0; // PID
    pdu[used++] = 0; // GSM 7-bit DCS
    pdu[used++] = length;
    for (size_t i = 0; i < length; i++) {
        size_t bit = i * 7;
        unsigned shifted = (unsigned char)message[i] << (bit % 8);
        pdu[used + bit / 8] |= shifted & 0xff;
        if (bit % 8 > 1) {
            pdu[used + bit / 8 + 1] |= shifted >> 8;
        }
    }
    used += ((length) * 7 + 7) / 8;
    char text[2 * sizeof(pdu) + 1];
    for (size_t i = 0; i < used; i++) {
        text[2 * i] = hex[pdu[i] >> 4];
        text[2 * i + 1] = hex[pdu[i] & 0xf];
    }
    text[2 * used] = '\0';
    modem_result_t result;
    return modem_submit_sms(modem, used - 1, text, &result);
}
#endif

void sms_control_start(sms_control_t *sms, modem_t *modem)
{
#ifdef CONFIG_ROADHOG_SMS_CONTROL
    sms->owner = owner;
    if (!sms_number_valid(owner) || !sms_users_load(sms)) {
        ESP_LOGE(TAG, "SMS owner or user storage is unavailable");
        return;
    }
    // Accept deliveries queued during the CNMI command itself.
    sms->configured = command_ok(modem, "ATE0") && command_ok(modem, "AT+CMGF=0") && command_ok(modem, "AT+CSMS=1");
    sms->configured = sms->configured && command_ok(modem, "AT+CNMI=2,2,0,0,0");
    if (sms->configured) {
        unsigned users = 0;
        for (size_t i = 0; i < SMS_USER_LIMIT; i++) {
            users += sms->users[i][0] != '\0';
        }
        ESP_LOGI(TAG, "SMS commands enabled; loaded %u additional users", users);
        start_gps(modem);
    } else {
        ESP_LOGE(TAG, "SMS setup failed");
    }
#else
    (void)sms;
    (void)modem;
#endif
}

bool sms_control_notification(sms_control_t *sms, const char *line)
{
    // Suppress headers and their payload even when SMS control is disabled.
    if (strncmp(line, "+CMT:", 5) == 0) {
        sms->expected_tpdu = 0;
        const char *separator = strrchr(line, ',');
        if (separator) {
            char *end;
            unsigned long length = strtoul(separator + 1, &end, 10);
            if (!*end && length > 0 && length <= 176) {
                sms->expected_tpdu = length;
            }
        }
        return true;
    }

    if (!sms->expected_tpdu) {
        return false;
    }
    size_t length = strlen(line);
    size_t tpdu_length = sms->expected_tpdu;
    sms->expected_tpdu = 0;
#ifdef CONFIG_ROADHOG_SMS_CONTROL
    if (sms->configured) {
        if (sms->pending_acks < UINT_MAX) {
            sms->pending_acks++;
        }
        uint8_t pdu[MAX_PDU_BYTES];
        sms_request_t request;
        if (length >= 2 && length % 2 == 0 && length / 2 <= sizeof(pdu) &&
            decode_hex(line, pdu, length / 2) &&
            decode_request(pdu, length / 2, tpdu_length, &request) &&
            sms_sender_allowed(sms, request.sender)) {
            if (sms->request_count == SMS_QUEUE_CAPACITY) {
                ESP_LOGW(TAG, "SMS command queue full");
            } else {
                size_t tail = (sms->request_head + sms->request_count) % SMS_QUEUE_CAPACITY;
                sms->requests[tail] = request;
                sms->request_count++;
            }
        }
    }
#else
    (void)length;
    (void)tpdu_length;
#endif
    return true;
}

void sms_control_service(sms_control_t *sms, modem_t *modem)
{
#ifdef CONFIG_ROADHOG_SMS_CONTROL
    if (!sms->configured) {
        return;
    }
    while (sms->pending_acks) {
        sms->pending_acks--;
        if (!command_ok(modem, "AT+CNMA")) {
            ESP_LOGW(TAG, "SMS acknowledgement failed");
            sms->request_count = 0;
            return;
        }
    }
    if (sms->request_count) {
        sms_request_t request = sms->requests[sms->request_head];
        sms->request_head = (sms->request_head + 1) % SMS_QUEUE_CAPACITY;
        sms->request_count--;
        if (sms_sender_allowed(sms, request.sender)) {
            char reply[SMS_TEXT_CAPACITY];
            sms_command_reply(sms, modem, &request, reply);
            if (send_reply(modem, request.sender, reply) == ESP_OK) {
                ESP_LOGI(TAG, "SMS reply accepted by modem");
            } else {
                ESP_LOGW(TAG, "SMS reply failed");
            }
        }
    }
#else
    (void)sms;
    (void)modem;
#endif
}
