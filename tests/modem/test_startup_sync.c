#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_ota_ops.h"
#include "ota_boot_health.h"

typedef int uart_port_t;
typedef void *QueueHandle_t;
typedef struct { int type; } uart_event_t;
typedef struct {
    int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk;
} uart_config_t;

enum {
    ESP_ERR_TIMEOUT = 100, ESP_ERR_INVALID_STATE,
    ESP_ERR_NO_MEM, ESP_ERR_INVALID_SIZE, ESP_ERR_INVALID_RESPONSE,
    UART_FIFO_OVF, UART_BUFFER_FULL, UART_FRAME_ERR, UART_PARITY_ERR, UART_BREAK,
    UART_DATA_8_BITS, UART_PARITY_DISABLE, UART_STOP_BITS_1, UART_HW_FLOWCTRL_DISABLE,
    UART_SCLK_DEFAULT, UART_PIN_NO_CHANGE, UART_NUM_MAX, pdTRUE,
};
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)

bool uart_is_driver_installed(uart_port_t port);
esp_err_t uart_param_config(uart_port_t port, const uart_config_t *config);
esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts);
esp_err_t uart_driver_install(uart_port_t port, int rx, int tx, int count, QueueHandle_t *queue, int flags);
esp_err_t uart_driver_delete(uart_port_t port);
static int64_t esp_timer_get_time(void);
static int xQueueReceive(QueueHandle_t queue, uart_event_t *event, int ticks);
static int uart_read_bytes(uart_port_t port, uint8_t *byte, int size, int ticks);
static int uart_write_bytes(uart_port_t port, const char *data, size_t size);
static esp_err_t uart_wait_tx_done(uart_port_t port, unsigned ticks);
esp_err_t uart_flush_input(uart_port_t port);
static void vTaskDelay(unsigned ticks);

#include "modem_header.inc"
#include "modem_source.inc"

typedef enum {
    HEALTHY, STALE_PPP, BARE_OK, NO_ESCAPE, HANGUP_FAILED, FINAL_PROBE_FAILED,
    UART_FAULT, LATE, TRANSIENT, INVALID_TYPED, DROPPED, STALE_RETRY, STALE_ONLY, SILENT, STALE_AFTER_PAYLOAD, STALE_ESCAPE,
} case_t;
static case_t scenario;
static int64_t elapsed, last_write, response_at;
static const char *incoming;
static unsigned queries, escapes, hangups, notifications;
static bool fault;
enum { SMS_IDLE, SMS_DENY_PROMPT, SMS_DENY_CLAIM, SMS_LOSE_FINAL, SMS_SUCCESS };
static unsigned sms_phase, sms_claims, sms_payloads, sms_escapes;
static const esp_partition_t boot_slot = {.subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0, .label = "ota_0"};
static esp_ota_img_states_t boot_state;
static unsigned marks, rollbacks;

const esp_partition_t *esp_ota_get_running_partition(void) { return &boot_slot; }
const esp_partition_t *esp_ota_get_boot_partition(void) { return &boot_slot; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *part, esp_ota_img_states_t *state)
{
    assert(part == &boot_slot);
    *state = boot_state;
    return ESP_OK;
}
bool esp_ota_check_rollback_is_possible(void) { return true; }
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    ++marks;
    boot_state = ESP_OTA_IMG_VALID;
    return ESP_OK;
}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void)
{
    ++rollbacks;
    return ESP_FAIL;
}

static int64_t esp_timer_get_time(void) { return elapsed; }
static void vTaskDelay(unsigned ticks) { elapsed += (int64_t)ticks * 1000; }

static int xQueueReceive(QueueHandle_t queue, uart_event_t *event, int ticks)
{
    (void)queue;
    (void)ticks;
    if (fault) {
        fault = false;
        event->type = UART_FRAME_ERR;
        return pdTRUE;
    }
    return 0;
}

static int uart_read_bytes(uart_port_t port, uint8_t *byte, int size, int ticks)
{
    assert(port == 1 && size == 1 && ticks == 1);
    elapsed += 1000;
    if (elapsed < response_at || !*incoming) {
        return 0;
    }
    *byte = (uint8_t)*incoming++;
    return 1;
}

static esp_err_t uart_wait_tx_done(uart_port_t port, unsigned ticks)
{
    assert(port == 1 && ticks == 200);
    return ESP_OK;
}

esp_err_t uart_flush_input(uart_port_t port)
{
    assert(port == 1);
    incoming = "";
    return ESP_OK;
}

static int uart_write_bytes(uart_port_t port, const char *data, size_t size)
{
    assert(port == 1 && !*incoming);
    if (sms_phase != SMS_IDLE) {
        if (size == sizeof("AT+CMGS=1\r") - 1 && !memcmp(data, "AT+CMGS=1\r", size)) {
            incoming = sms_phase == SMS_DENY_PROMPT ? "\r\n+CMS ERROR: 500\r\n" : "\r\n> ";
        } else if (size == 1 && data[0] == 0x1b) {
            assert(sms_phase == SMS_DENY_CLAIM && sms_claims == 1 && !sms_payloads);
            ++sms_escapes;
            incoming = "\r\nOK\r\n";
        } else {
            assert(sms_claims == 1 && size == 5 && !memcmp(data, "0000\x1a", size));
            ++sms_payloads;
            incoming = sms_phase == SMS_LOSE_FINAL ? "" : "\r\n+CMGS: 1\r\nOK\r\n";
        }
        response_at = elapsed;
        return size;
    }
    if (size == 3 && !memcmp(data, "+++", 3)) {
        assert(++escapes == 1 && elapsed - last_write >= 1200000);
        if (scenario == STALE_ESCAPE) {
            // This belongs to an earlier probe. The escape emits no ACK.
            response_at = elapsed + 1300000;
            incoming = "\r\nOK\r\n";
        } else if (scenario != NO_ESCAPE && scenario != SILENT && scenario != STALE_ONLY) {
            incoming = "\r\nOK\r\n";
        }
    } else if (size == 9 && (!memcmp(data, "AT+CMGF?\r", 9) || !memcmp(data, "AT+CMEE?\r", 9))) {
        bool retry = data[6] == 'E';
        ++queries;
        unsigned check_probe = hangups ? queries - 3 : escapes ? queries - 2 : queries;
        assert(retry == (check_probe == 2));
        if (escapes && !hangups && check_probe == 1) {
            assert(elapsed - last_write >= 1200000);
        }
        if (scenario == TRANSIENT && queries == 1) {
            incoming = "\r\nERROR\r\n";
        } else if (scenario == LATE && queries == 1) {
            response_at = elapsed + 2250000;
            incoming = "\r\n+CMGF: 1\r\nOK\r\n";
        } else if ((scenario == DROPPED || scenario == STALE_RETRY || scenario == STALE_ONLY) && !retry) {
            // The first query is dropped. In DROPPED, an unrelated URC comes
            // in after the deadline but does not terminate that transaction.
            response_at = elapsed + 2100000;
            incoming = scenario == DROPPED ? "\r\n+CEREG: 1\r\n" : "";
        } else if (scenario == STALE_RETRY || scenario == STALE_ONLY) {
            assert(retry && elapsed - last_write >= 2100000);
            incoming = scenario == STALE_RETRY
                       ? "\r\n+CMGF: 1\r\nOK\r\nERROR\r\n+CEREG: 1\r\n+CMEE: 2\r\nOK\r\n"
                       : "\r\n+CMGF: 1\r\nOK\r\nERROR\r\n";
        } else if (scenario == STALE_AFTER_PAYLOAD && retry) {
            // The new query sends its payload only. The retired query sends OK.
            incoming = "\r\n+CMEE: 2\r\nOK\r\n";
        } else if (!escapes && (scenario == STALE_PPP || scenario == HANGUP_FAILED ||
                                scenario == FINAL_PROBE_FAILED || scenario == NO_ESCAPE)) {
            incoming = "\r\nERROR\r\n";
        } else if (scenario == HEALTHY || scenario == LATE || scenario == TRANSIENT || scenario == DROPPED ||
                   (escapes && scenario != BARE_OK && scenario != INVALID_TYPED &&
                    !(scenario == FINAL_PROBE_FAILED && hangups))) {
            incoming = retry ? "\r\n+CMEE: 2\r\nOK\r\n"
                             : "\r\n +CMGF:\t1 \t\r\n+CEREG: 1\r\n  OK \t\r\n";
        } else if (scenario == BARE_OK) {
            incoming = "\r\nOK\r\n";
        } else if (scenario == INVALID_TYPED) {
            incoming = retry ? "\r\n+CMEE: 3\r\nOK\r\n" : "\r\n+CMGF: 10\r\nOK\r\n";
        } else if (scenario != SILENT) {
            incoming = "partial PPP data without a final";
        }
    } else if (size == 9 && !memcmp(data, "AT+CFUN?\r", 9)) {
        assert(++queries == 3);
        if (scenario == DROPPED || scenario == STALE_RETRY) {
            incoming = "\r\n+CFUN: 1\r\nOK\r\n";
        } else {
            assert(scenario == STALE_AFTER_PAYLOAD);
        }
    } else if (size == 8 && !memcmp(data, "AT+CGSN\r", 8)) {
        incoming = "\r\n123456789012345\r\nOK\r\n";
    } else {
        assert(size == 4 && !memcmp(data, "ATH\r", 4));
        assert(escapes == 1 && queries == 3 && ++hangups == 1);
        incoming = scenario == HANGUP_FAILED ? "\r\nERROR\r\n" : "\r\nOK\r\n";
    }
    last_write = elapsed;
    return (int)size;
}

static void notify(const char *line, void *context)
{
    (void)line;
    (void)context;
    ++notifications;
}

static esp_err_t run_case(case_t test, esp_err_t expected, unsigned expected_queries,
                     unsigned expected_escapes, unsigned expected_hangups)
{
    modem_t modem = {.config = {.port = 1, .on_notification = notify}};
    scenario = test;
    elapsed = last_write = response_at = queries = escapes = hangups = notifications = 0;
    incoming = "";
    fault = test == UART_FAULT;
    esp_err_t result = modem_sync_startup(&modem);
    assert(result == expected);
    assert(queries == expected_queries && escapes == expected_escapes && hangups == expected_hangups);
    assert(!notifications && modem.config.on_notification == notify);
    assert(elapsed < 30000000);
    if (result == ESP_OK) {
        assert(modem_maintenance_ready(&modem));
    } else if (test == FINAL_PROBE_FAILED || test == STALE_AFTER_PAYLOAD || test == STALE_ESCAPE) {
        assert(!modem_maintenance_ready(&modem));
    }
    return result;
}

static void boot_case(esp_ota_img_states_t initial, case_t test, bool other_checks_pass,
                      unsigned expected_marks, unsigned expected_rollbacks)
{
    boot_state = initial;
    marks = rollbacks = 0;
    bool recovers = test == DROPPED;
    esp_err_t startup = run_case(test, recovers ? ESP_OK : ESP_ERR_INVALID_STATE,
                                 test == STALE_ESCAPE ? 2 : 3, 0, 0);
    ota_boot_health_finish(startup == ESP_OK && other_checks_pass);
    assert(marks == expected_marks && rollbacks == expected_rollbacks);
    assert(boot_state == (expected_marks ? ESP_OTA_IMG_VALID : initial));
}

static esp_err_t test_sms_claim(void *context)
{
    assert(context == &sms_phase && !sms_payloads);
    ++sms_claims;
    return sms_phase == SMS_DENY_CLAIM ? ESP_FAIL : ESP_OK;
}

static void test_sms_submission(unsigned phase, esp_err_t expected)
{
    modem_t modem = {.config = {.port = 1}};
    modem_result_t result;
    incoming = "";
    sms_phase = phase;
    sms_claims = sms_payloads = sms_escapes = 0;
    assert(modem_submit_sms_claimed(&modem, 1, "0000", &result, test_sms_claim, &sms_phase) == expected);
    assert(sms_claims == (unsigned)(phase != SMS_DENY_PROMPT));
    assert(sms_payloads == (unsigned)(phase == SMS_LOSE_FINAL || phase == SMS_SUCCESS));
    assert(sms_escapes == (unsigned)(phase == SMS_DENY_CLAIM));
    assert(modem_maintenance_ready(&modem) == (phase != SMS_LOSE_FINAL));
    sms_phase = SMS_IDLE;
}

int main(void)
{
    run_case(HEALTHY, ESP_OK, 1, 0, 0);
    run_case(STALE_PPP, ESP_OK, 4, 1, 1);
    run_case(BARE_OK, ESP_ERR_INVALID_STATE, 4, 1, 0);
    run_case(NO_ESCAPE, ESP_ERR_INVALID_STATE, 2, 1, 0);
    run_case(HANGUP_FAILED, ESP_ERR_INVALID_STATE, 3, 1, 1);
    run_case(FINAL_PROBE_FAILED, ESP_ERR_INVALID_STATE, 5, 1, 1);
    run_case(UART_FAULT, ESP_FAIL, 0, 0, 0);
    run_case(LATE, ESP_OK, 2, 0, 0);
    run_case(TRANSIENT, ESP_OK, 2, 0, 0);
    run_case(INVALID_TYPED, ESP_ERR_INVALID_STATE, 4, 1, 0);
    run_case(DROPPED, ESP_OK, 3, 0, 0);
    run_case(STALE_RETRY, ESP_OK, 3, 0, 0);
    run_case(STALE_ONLY, ESP_ERR_INVALID_STATE, 2, 0, 0);
    run_case(SILENT, ESP_ERR_INVALID_STATE, 2, 0, 0);
    boot_case(ESP_OTA_IMG_VALID, DROPPED, true, 0, 0);
    boot_case(ESP_OTA_IMG_VALID, STALE_AFTER_PAYLOAD, true, 0, 0);
    boot_case(ESP_OTA_IMG_VALID, STALE_ESCAPE, true, 0, 0);
    boot_case(ESP_OTA_IMG_PENDING_VERIFY, DROPPED, true, 1, 0);
    boot_case(ESP_OTA_IMG_PENDING_VERIFY, DROPPED, false, 0, 1);
    boot_case(ESP_OTA_IMG_PENDING_VERIFY, STALE_AFTER_PAYLOAD, true, 0, 1);
    boot_case(ESP_OTA_IMG_PENDING_VERIFY, STALE_ESCAPE, true, 0, 1);
    modem_t modem = {.config = {.port = 1, .on_notification = notify}};
    char identity[20];
    modem_result_t result;
    assert(modem_maintenance_ready(&modem));
    modem.waiting_for_final = true;
    assert(!modem_maintenance_ready(&modem));
    assert(modem_sync_startup(&modem) == ESP_ERR_INVALID_STATE);
    assert(modem_command(&modem, "AT+CGSN", "+CGSN:", 1000, identity, sizeof(identity), &result) ==
           ESP_ERR_INVALID_STATE);
    assert(modem.waiting_for_final);
    modem.waiting_for_final = false;
    assert(modem_command(&modem, "AT+CGSN", "+CGSN:", 1000, identity, sizeof(identity), &result) == ESP_OK);
    assert(!strcmp(identity, "123456789012345\n") && result.final == MODEM_FINAL_OK && !notifications);
    test_sms_submission(SMS_DENY_PROMPT, ESP_ERR_INVALID_RESPONSE);
    test_sms_submission(SMS_DENY_CLAIM, ESP_FAIL);
    test_sms_submission(SMS_LOSE_FINAL, ESP_ERR_TIMEOUT);
    test_sms_submission(SMS_SUCCESS, ESP_OK);
    puts("Modem startup, stale finals, boot health, private identity and SMS submission phases passed");
}
