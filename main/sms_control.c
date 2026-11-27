#include "sms_control.h"
#include "ota_update.h"
#include "sms_commands.h"
#include "provisioning.h"
#include "owner_verification.h"

#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/platform_util.h"
#include "sdkconfig.h"

static const char hex[] = "0123456789ABCDEF";

enum { MAX_PDU_BYTES = 190 };

static bool command_ok(modem_t *modem, const char *command)
{
    modem_result_t result;
    return modem_command(modem, command, NULL, 5000, NULL, 0, &result) == ESP_OK &&
           result.final == MODEM_FINAL_OK;
}

#ifdef CONFIG_ROADHOG_SMS_CONTROL
static const char *TAG = "sms";

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

#endif

esp_err_t sms_control_send_claimed(modem_t *modem, const char *number, const char *message,
                                  modem_sms_claim_fn claim, void *context)
{
    if (!sms_number_valid(number) || strlen(message) >= SMS_TEXT_CAPACITY) {
        return ESP_ERR_INVALID_ARG;
    }
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
    esp_err_t error = claim ? modem_submit_sms_claimed(modem, used - 1, text, &result, claim, context)
                            : modem_submit_sms(modem, used - 1, text, &result);
    mbedtls_platform_zeroize(text, sizeof(text));
    mbedtls_platform_zeroize(pdu, sizeof(pdu));
    return error;
}

esp_err_t sms_control_send(modem_t *modem, const char *number, const char *message)
{
    return sms_control_send_claimed(modem, number, message, NULL, NULL);
}

void sms_owner_verification_service(modem_t *modem)
{
    owner_verify_message_t request;
    if (!owner_verification_take(&request)) {
        return;
    }
    char message[48];
    snprintf(message, sizeof(message), "Road Hog owner verification code: %s", request.code);
    bool accepted = command_ok(modem, "ATE0") && command_ok(modem, "AT+CMGF=0") &&
                    sms_control_send(modem, request.owner, message) == ESP_OK;
    owner_verification_finish(accepted);
    mbedtls_platform_zeroize(&request, sizeof(request));
    mbedtls_platform_zeroize(message, sizeof(message));
}

#ifdef CONFIG_ROADHOG_SMS_CONTROL
static bool cnmi_verified(modem_t *modem, unsigned expected_mode, unsigned expected_mt)
{
    char reply[48];
    modem_result_t result;
    unsigned mode, mt, bm, ds, bfr;
    int end = 0;
    return modem_command(modem, "AT+CNMI?", "+CNMI:", 5000, reply, sizeof(reply), &result) == ESP_OK &&
           result.final == MODEM_FINAL_OK &&
           sscanf(reply, "+CNMI: %u,%u,%u,%u,%u%n", &mode, &mt, &bm, &ds, &bfr, &end) == 5 &&
           !strcmp(reply + end, "\n") && mode == expected_mode && mt == expected_mt && !bm && !ds && !bfr;
}

static void configure_sms(sms_control_t *sms, modem_t *modem)
{
    // Accept deliveries queued during the CNMI command itself.
    sms->configured = command_ok(modem, "ATE0") && command_ok(modem, "AT+CMGF=0") && command_ok(modem, "AT+CSMS=1");
    sms->configured = sms->configured && command_ok(modem, "AT+CNMI=2,2,0,0,0") &&
                      (!sms->routing_recovery || cnmi_verified(modem, 2, 2));
}
#endif

void sms_control_start(sms_control_t *sms, modem_t *modem)
{
#ifdef CONFIG_ROADHOG_SMS_CONTROL
    sms_users_lock_init();
    sms->owner = provisioning_owner();
    if (!sms->owner) {
        return;
    }
    if (!sms_users_load(sms)) {
        ESP_LOGE(TAG, "SMS owner or user storage is unavailable");
        return;
    }
    configure_sms(sms, modem);
#if defined(ROADHOG_SMS_OTA_DESCRIPTOR) || defined(CONFIG_ROADHOG_OTA_SMS_BENCH)
    // A prior SMS OTA boot may have left deliveries in the modem's ME store.
    sms->storage_pending = true;
    sms->storage_startup = true;
#endif
    if (sms->configured) {
        unsigned users = 0;
        for (size_t i = 0; i < SMS_USER_LIMIT; i++) {
            users += sms->users[i][0] != '\0';
        }
        ESP_LOGI(TAG, "SMS commands enabled; loaded %u additional users", users);
    } else {
        ESP_LOGE(TAG, "SMS setup failed");
    }
#else
    (void)sms;
    (void)modem;
#endif
}

void sms_control_resume(sms_control_t *sms, modem_t *modem)
{
#ifdef CONFIG_ROADHOG_SMS_CONTROL
    if (sms->configured || sms->resume_at) {
        sms_control_expire_pdu(sms);
        configure_sms(sms, modem);
        sms->resume_at = sms->configured ? 0 : esp_timer_get_time() + INT64_C(5000000);
        if (sms->configured) {
            ESP_LOGI(TAG, "SMS commands resumed after LTE diagnostic");
        } else {
            ESP_LOGE(TAG, "SMS setup failed after LTE diagnostic");
        }
    }
#else
    (void)sms;
    (void)modem;
#endif
}

static void sms_control_delivery_fault(sms_control_t *sms)
{
    sms->delivery_fault = true;
    if (sms->pending_nacks < UINT_MAX) {
        ++sms->pending_nacks;
    }
}

void sms_control_expire_pdu(sms_control_t *sms)
{
    if (sms->expected_tpdu && esp_timer_get_time() >= sms->tpdu_deadline) {
        sms->expected_tpdu = 0;
        if (sms->configured) {
            sms_control_delivery_fault(sms);
        }
    }
}

#if defined(CONFIG_ROADHOG_SMS_CONTROL)
static bool read_me_capacity(modem_t *modem, unsigned *used, unsigned *capacity)
{
    char reply[96];
    modem_result_t result;
    unsigned write_used, write_total, incoming_used, incoming_total;
    int end = 0;
    return modem_command(modem, "AT+CPMS?", "+CPMS:", 5000, reply, sizeof(reply), &result) == ESP_OK &&
           result.final == MODEM_FINAL_OK &&
           sscanf(reply, "+CPMS: \"ME\",%u,%u,\"ME\",%u,%u,\"ME\",%u,%u%n",
                  used, capacity, &write_used, &write_total, &incoming_used, &incoming_total, &end) == 6 &&
           !strcmp(reply + end, "\n") && *capacity && *used <= *capacity &&
           write_total == *capacity && incoming_total == *capacity &&
           write_used <= *capacity && incoming_used <= *capacity;
}

static bool read_inbox(sms_control_t *sms, modem_t *modem)
{
    modem_result_t result;
    sms->inbox_found = false;
    sms->inbox_length = 0;
    sms->inbox_valid = false;
    sms->inbox_error = false;
    sms->scanning_inbox = true;
    esp_err_t error = modem_command(modem, "AT+CMGL=4", NULL, 5000, NULL, 0, &result);
    sms->scanning_inbox = false;
    return error == ESP_OK && result.final == MODEM_FINAL_OK &&
           !sms->inbox_error && !sms->inbox_length;
}

static void queue_request(sms_control_t *sms, const sms_request_t *request)
{
    size_t tail = (sms->request_head + sms->request_count) % SMS_QUEUE_CAPACITY;
    sms->requests[tail] = *request;
    ++sms->request_count;
}

static bool inbox_line(sms_control_t *sms, const char *line)
{
    if (!sms->scanning_inbox) {
        return false;
    }
    if (!strncmp(line, "+CMGL:", 6)) {
        unsigned index, status;
        const char *last = strrchr(line, ',');
        char *end;
        unsigned long length = last ? strtoul(last + 1, &end, 10) : 0;
        if (sms->inbox_length ||
            sscanf(line, "+CMGL: %u,%u,", &index, &status) != 2 ||
            status > 3 || !length || length > 176 || *end) {
            sms->inbox_error = true;
        } else if (!sms->inbox_found && status <= 1) {
            sms->inbox_found = true;
            sms->inbox_index = index;
            sms->inbox_length = length;
        }
        return true;
    }
    size_t length = strlen(line);
    if (sms->inbox_length && length >= 2 && length % 2 == 0 &&
        strspn(line, "0123456789ABCDEFabcdef") == length) {
        uint8_t pdu[MAX_PDU_BYTES];
        if (length / 2 > sizeof(pdu) || !decode_hex(line, pdu, length / 2) ||
            pdu[0] + 1u + sms->inbox_length != length / 2) {
            sms->inbox_error = true;
        } else {
            sms->inbox_valid = decode_request(pdu, length / 2, sms->inbox_length,
                                             &sms->inbox_request);
        }
        sms->inbox_length = 0;
        return true;
    }
    return false;
}

static bool direct_delivery_idle(sms_control_t *sms, modem_t *modem)
{
    return modem_maintenance_ready(modem) && !sms->expected_tpdu &&
           !sms->pending_acks && !sms->pending_nacks && !sms->delivery_fault;
}

static bool pause_direct(sms_control_t *sms, modem_t *modem)
{
    return command_ok(modem, "AT+CPMS=\"ME\",\"ME\",\"ME\"") &&
           command_ok(modem, "AT+CNMI=0,1,0,0,0") && cnmi_verified(modem, 0, 1) &&
           modem_poll(modem, 250) == ESP_OK && direct_delivery_idle(sms, modem);
}

static void service_inbox(sms_control_t *sms, modem_t *modem)
{
    unsigned used, capacity;
    if (pause_direct(sms, modem) && read_me_capacity(modem, &used, &capacity) &&
        read_inbox(sms, modem) && direct_delivery_idle(sms, modem)) {
        if (!sms->inbox_found) {
            sms->storage_pending = false;
        } else {
            char command[24];
            snprintf(command, sizeof(command), "AT+CMGD=%u", sms->inbox_index);
            // Consume by index before dispatch. A reset after deletion can lose
            // the delivery, while a failed deletion never executes a command.
            if (!command_ok(modem, command)) {
                // The modem may have deleted a cancellation before losing
                // its final. An empty rescan cannot reauthorize this update.
                sms->delivery_fault = true;
#ifdef CONFIG_ROADHOG_OTA_SMS_BENCH
                ota_sms_ack_failed();
#endif
            } else if (sms->inbox_valid && sms_sender_allowed(sms, sms->inbox_request.sender)) {
                if (sms->request_count < SMS_QUEUE_CAPACITY) {
                    queue_request(sms, &sms->inbox_request);
                } else {
                    sms->delivery_fault = true;
                }
            }
        }
    }
    sms_control_resume(sms, modem);
    if (!sms->storage_pending) {
        if (sms->configured && (!sms->routing_recovery || direct_delivery_idle(sms, modem))) {
            sms->storage_startup = false;
            sms->routing_recovery = false;
        } else {
            sms->storage_pending = true;
        }
    }
}
#endif

#if defined(CONFIG_ROADHOG_OTA_SMS_BENCH)
bool sms_control_quiesce(sms_control_t *sms, modem_t *modem)
{
    // SIM7600 mode 0 is compatible with stored delivery (mt=1). The inbox
    // survives UART closure and remains authoritative if an indication drops.
    unsigned used, capacity;
    sms->storage_pending = true;
    if (!pause_direct(sms, modem) || !read_me_capacity(modem, &used, &capacity) ||
        used == capacity || !read_inbox(sms, modem) || sms->inbox_found ||
        !direct_delivery_idle(sms, modem) || sms->request_count) {
        sms_control_resume(sms, modem);
        return false;
    }
    return true;
}
#endif

bool sms_control_notification(sms_control_t *sms, const char *line)
{
    sms_control_expire_pdu(sms);
#ifdef CONFIG_ROADHOG_SMS_CONTROL
    if (!strncmp(line, "+CMTI:", 6)) {
        const char *store = line + 6;
        store += strspn(store, " ");
        if (!strncmp(store, "\"ME\",", 5)) {
            sms->storage_pending = true;
        }
        return true;
    }
    if ((!sms->expected_tpdu || !strncmp(line, "+CMGL:", 6)) && inbox_line(sms, line)) {
        return true;
    }
#endif
    // Suppress headers and their payload even when SMS control is disabled.
    if (strncmp(line, "+CMT:", 5) == 0) {
        if (sms->expected_tpdu && sms->configured) {
            sms_control_delivery_fault(sms);
        }
        sms->expected_tpdu = 0;
        const char *separator = strrchr(line, ',');
        if (separator) {
            char *end;
            unsigned long length = strtoul(separator + 1, &end, 10);
            if (!*end && length > 0 && length <= 176) {
                sms->expected_tpdu = length;
                sms->tpdu_deadline = esp_timer_get_time() + INT64_C(1000000);
            }
        }
        if (!sms->expected_tpdu && sms->configured) {
            sms_control_delivery_fault(sms);
        }
        return true;
    }

    size_t length = strlen(line);
    bool hex_line = length >= 2 && strspn(line, "0123456789ABCDEFabcdef") == length;
    if (!sms->expected_tpdu || !hex_line) {
        // Suppress orphan PDU bytes without accepting them as a command.
        return hex_line;
    }
    size_t tpdu_length = sms->expected_tpdu;
    sms->expected_tpdu = 0;
#ifdef CONFIG_ROADHOG_SMS_CONTROL
    if (sms->configured) {
        uint8_t pdu[MAX_PDU_BYTES];
        sms_request_t request;
        bool framed = length % 2 == 0 && length / 2 <= sizeof(pdu) &&
                      decode_hex(line, pdu, length / 2) && pdu[0] + 1u + tpdu_length == length / 2;
        bool authorized = framed && decode_request(pdu, length / 2, tpdu_length, &request) &&
                          sms_sender_allowed(sms, request.sender);
        if (!framed || sms->pending_nacks || (authorized && sms->request_count == SMS_QUEUE_CAPACITY)) {
            sms_control_delivery_fault(sms);
        } else {
            if (sms->pending_acks < UINT_MAX) {
                ++sms->pending_acks;
            }
            if (authorized) {
                queue_request(sms, &request);
            }
        }
    }
#else
    (void)tpdu_length;
#endif
    return true;
}

void sms_control_service(sms_control_t *sms, modem_t *modem)
{
    sms_control_expire_pdu(sms);
#ifdef CONFIG_ROADHOG_SMS_CONTROL
    if (sms->resume_at && esp_timer_get_time() >= sms->resume_at &&
        modem_maintenance_ready(modem)) {
        sms_control_resume(sms, modem);
    }
    if (!sms->configured) {
        return;
    }
    if (sms->delivery_fault) {
#ifdef CONFIG_ROADHOG_OTA_SMS_BENCH
        ota_sms_ack_failed();
#endif
        sms->delivery_fault = false;
    }
    for (unsigned i = 0; i < SMS_QUEUE_CAPACITY &&
                         (sms->pending_acks || sms->pending_nacks); ++i) {
        bool positive = sms->pending_acks != 0;
        if (positive) {
            --sms->pending_acks;
        } else {
            --sms->pending_nacks;
        }
        if (!command_ok(modem, positive ? "AT+CNMA" : "AT+CNMA=2")) {
            ESP_LOGW(TAG, "SMS acknowledgement failed");
            // The modem may have timed out the delivery and reset CNMI.
            // Outstanding direct acknowledgements cannot be retried blindly.
            sms->request_count = 0;
            sms->pending_acks = 0;
            sms->pending_nacks = 0;
            sms->expected_tpdu = 0;
            sms->configured = false;
            sms->routing_recovery = true;
            sms->storage_pending = true;
            sms->resume_at = esp_timer_get_time() + INT64_C(5000000);
#ifdef CONFIG_ROADHOG_OTA_SMS_BENCH
            ota_sms_ack_failed();
#endif
            return;
        }
    }
    if (sms->pending_acks || sms->pending_nacks || sms->delivery_fault) {
        return;
    }
    if (sms->storage_pending && !sms->request_count && !sms->expected_tpdu) {
        service_inbox(sms, modem);
    }
    if (sms->configured && !sms->pending_acks && !sms->pending_nacks &&
        !sms->delivery_fault && sms->request_count) {
        sms_request_t request = sms->requests[sms->request_head];
        sms->request_head = (sms->request_head + 1) % SMS_QUEUE_CAPACITY;
        sms->request_count--;
        if (sms_sender_allowed(sms, request.sender)) {
#ifdef CONFIG_ROADHOG_OTA_SMS_BENCH
            if (ota_sms_service_command(sms, modem, &request)) {
                return;
            }
#endif
            char reply[SMS_TEXT_CAPACITY];
            sms_command_reply(sms, modem, &request, reply);
            if (sms_control_send(modem, request.sender, reply) == ESP_OK) {
                ESP_LOGI(TAG, "SMS reply accepted by modem");
            } else {
                ESP_LOGW(TAG, "SMS reply failed");
            }
            mbedtls_platform_zeroize(reply, sizeof(reply));
        }
    }
#else
    (void)sms;
    (void)modem;
#endif
}
