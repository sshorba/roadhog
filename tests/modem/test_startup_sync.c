#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_ota_ops.h"
#include "ota_boot_health.h"
#include "modem_lifecycle.h"

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
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
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
#include "connection_status_header.inc"
#include "connection_status_source.inc"
#include "online_status.inc"

typedef enum {
    HEALTHY, STALE_PPP, BARE_OK, NO_ESCAPE, HANGUP_FAILED, FINAL_PROBE_FAILED,
    UART_FAULT, LATE, TRANSIENT, INVALID_TYPED, DROPPED, STALE_RETRY, STALE_ONLY, SILENT, STALE_AFTER_PAYLOAD, STALE_ESCAPE, STATUS_SAMPLE,
} case_t;
static case_t scenario;
static int64_t elapsed, last_write, response_at;
static const char *incoming;
static unsigned queries, escapes, hangups, notifications;
static bool fault;
static bool gps_sample_error, gps_sample_late, lte_registered;
static unsigned editor_aborts;
static bool driver_installed, fail_open;
static unsigned driver_opens, driver_closes;

bool uart_is_driver_installed(uart_port_t port)
{
    assert(port == 1);
    return driver_installed;
}

esp_err_t uart_param_config(uart_port_t port, const uart_config_t *config)
{
    assert(port == 1 && config->baud_rate == 115200);
    return ESP_OK;
}

esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts)
{
    assert(port == 1 && tx == 17 && rx == 18 &&
           rts == UART_PIN_NO_CHANGE && cts == UART_PIN_NO_CHANGE);
    return ESP_OK;
}

esp_err_t uart_driver_install(uart_port_t port, int rx, int tx, int count,
                             QueueHandle_t *queue, int flags)
{
    (void)rx;
    (void)tx;
    (void)count;
    (void)queue;
    (void)flags;
    assert(port == 1 && !driver_installed);
    if (fail_open) {
        return ESP_FAIL;
    }
    driver_installed = true;
    ++driver_opens;
    return ESP_OK;
}

esp_err_t uart_driver_delete(uart_port_t port)
{
    assert(port == 1 && driver_installed);
    driver_installed = false;
    ++driver_closes;
    return ESP_OK;
}

static const esp_partition_t boot_slot = {.subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0, .label = "ota_0"};
static esp_ota_img_states_t boot_state;
static unsigned marks, rollbacks, restarts;
static unsigned read_failures;
static bool lose_mark_readback;
static esp_err_t mark_error, rollback_error;
static bool fallback = true;
static const esp_partition_t fallback_slot = {.subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1,
                                               .label = "ota_1", .address = 0x410000};

const esp_partition_t *esp_ota_get_running_partition(void) { return &boot_slot; }
const esp_partition_t *esp_ota_get_boot_partition(void)
{
    return boot_state == ESP_OTA_IMG_INVALID && fallback ? &fallback_slot : &boot_slot;
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *part, esp_ota_img_states_t *state)
{
    assert(part == &boot_slot);
    if (read_failures) {
        --read_failures;
        return ESP_ERR_NOT_FOUND;
    }
    *state = boot_state;
    return ESP_OK;
}
bool esp_ota_check_rollback_is_possible(void) { return fallback; }
void esp_restart(void) { ++restarts; }
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    ++marks;
    if (mark_error == ESP_OK) {
        boot_state = ESP_OTA_IMG_VALID;
        if (lose_mark_readback) {
            ++read_failures;
            lose_mark_readback = false;
        }
    }
    return mark_error;
}
esp_err_t esp_ota_mark_app_invalid_rollback(void)
{
    ++rollbacks;
    if (rollback_error == ESP_OK) {
        boot_state = ESP_OTA_IMG_INVALID;
    }
    return rollback_error;
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
    if (size == 1 && data[0] == 0x1b) {
        ++editor_aborts;
        incoming = "\r\nERROR\r\n";
        return 1;
    }
    if (size == 10 && !memcmp(data, "AT+CMGS=1\r", size)) {
        return (int)size; // Its prompt will arrive after reconnect.
    }
    if (scenario == STATUS_SAMPLE) {
        if (size == 9 && !memcmp(data, "AT+CGPS?\r", size)) {
            incoming = gps_sample_error ? "\r\nERROR\r\n" : "\r\n+CGPS: 1,1\r\nOK\r\n";
            gps_sample_error = false;
            if (gps_sample_late) {
                response_at = elapsed + 3100000;
                gps_sample_late = false;
                return (int)size;
            }
        } else if (size == 10 && !memcmp(data, "AT+CEREG?\r", size)) {
            incoming = lte_registered ? "\r\n+CEREG: 0,1\r\nOK\r\n" : "\r\n+CEREG: 0,0\r\nOK\r\n";
        } else if (size == 10 && !memcmp(data, "AT+CGACT?\r", size)) {
            incoming = "\r\n+CGACT: 1,0\r\nOK\r\n";
        } else if (size == 12 && !memcmp(data, "AT+CGPSINFO\r", size)) {
            incoming = "\r\n+CGPSINFO: ,,,,,,,,\r\nOK\r\n";
        } else if (size == 9 && !memcmp(data, "AT+CPIN?\r", size)) {
            incoming = "\r\n+CPIN: READY\r\nOK\r\n";
        } else {
            assert(size == 7 && !memcmp(data, "AT+CSQ\r", size));
            incoming = "\r\n+CSQ: 12,99\r\nOK\r\n";
        }
        response_at = elapsed;
        return (int)size;
    }
    if (size == 7 && !memcmp(data, "AT+CSQ\r", size)) {
        return (int)size; // Late final is injected by the reconnect fixture.
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
    rollback_error = ESP_FAIL;
    bool recovers = test == DROPPED;
    esp_err_t startup = run_case(test, recovers ? ESP_OK : ESP_ERR_INVALID_STATE,
                                 test == STALE_ESCAPE ? 2 : 3, 0, 0);
    ota_boot_health_finish(startup == ESP_OK && other_checks_pass, startup != ESP_OK || !other_checks_pass);
    assert(marks == expected_marks && rollbacks == expected_rollbacks);
    assert(boot_state == (expected_marks ? ESP_OTA_IMG_VALID : initial));
}

typedef bool *SemaphoreHandle_t;
static int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned ticks)
{
    assert(ticks == 0);
    if (*semaphore) {
        *semaphore = false;
        return pdTRUE;
    }
    return 0;
}

#include "lte_uart_ownership.inc"

static void worker_handoff_case(void)
{
    bool completed = false;
    assert(!lte_session_owns_uart());
    worker_done = &completed;
    // A timed-out worker still owns UART until cleanup signals completion.
    assert(lte_session_owns_uart());
    assert(lte_session_owns_uart());
    completed = true;
    assert(!lte_session_owns_uart());
    assert(!completed && !lte_session_owns_uart());
}

static void lifecycle_cases(void)
{
    modem_lifecycle_t stable = {.health_deadline = INT64_MAX};
    // UART open failure / modem absent, then a slow modem becomes responsive.
    const modem_config_t config = {.port = 1, .tx_pin = 17, .rx_pin = 18, .baud_rate = 115200};
    modem_t *connection = NULL;
    fail_open = true;
    assert(modem_open(&config, &connection) == ESP_FAIL && !connection && !driver_installed);
    fail_open = false;
    assert(modem_lifecycle_begin(&stable, 0));
    assert(!modem_lifecycle_begin(&stable, 0));
    modem_lifecycle_result(&stable, false, 1000);
    assert(stable.phase == MODEM_RECOVERING);
    assert(!modem_lifecycle_begin(&stable, 5999));
    assert(modem_lifecycle_begin(&stable, 6000));
    assert(modem_open(&config, &connection) == ESP_OK);
    assert(run_case(SILENT, ESP_ERR_INVALID_STATE, 2, 0, 0) != ESP_OK);
    modem_close(connection);
    modem_lifecycle_result(&stable, false, 12000);
    assert(modem_lifecycle_begin(&stable, 17000));
    assert(run_case(HEALTHY, ESP_OK, 1, 0, 0) == ESP_OK);
    modem_lifecycle_result(&stable, true, 18000);
    assert(stable.phase == MODEM_ONLINE);
    // Network registration is not an input to lifecycle decisions.
    assert(!modem_lifecycle_begin(&stable, 30000));

    assert(modem_open(&config, &connection) == ESP_OK);
    modem_watch_restart(connection);
    modem_t *duplicate;
    assert(modem_open(&config, &duplicate) == ESP_ERR_INVALID_STATE && !duplicate);
    incoming = "\r\nRDY\r\nOK\r\n";
    response_at = elapsed;
    assert(modem_poll(connection, 1000) == ESP_FAIL);
    assert(!modem_maintenance_ready(connection));
    assert(!notifications);
    modem_close(connection);
    modem_lifecycle_result(&stable, false, 30000);
    assert(!modem_lifecycle_begin(&stable, 34999));
    assert(modem_lifecycle_begin(&stable, 35000));
    assert(modem_open(&config, &connection) == ESP_OK);
    assert(run_case(LATE, ESP_OK, 2, 0, 0) == ESP_OK);
    modem_lifecycle_result(&stable, true, 38000);
    assert(stable.phase == MODEM_ONLINE);
    modem_close(connection);
    assert(!driver_installed && driver_opens == 3 && driver_closes == 3);


}

static void reconnect_cases(void)
{
    const modem_config_t config = {.port = 1, .tx_pin = 17, .rx_pin = 18, .baud_rate = 115200,
                                   .on_notification = notify};
    modem_t *modem;
    modem_result_t result;
    for (unsigned sms = 0; sms <= 1; ++sms) {
        incoming = "";
        queries = notifications = 0;
        scenario = HEALTHY;
        assert(modem_open(&config, &modem) == ESP_OK);
        modem_watch_restart(modem);
        esp_err_t error = sms ? modem_submit_sms(modem, 1, "0000", &result) :
                               modem_command(modem, "AT+CSQ", NULL, 100, NULL, 0, &result);
        assert(error == ESP_ERR_TIMEOUT);
        modem_close(modem);
        assert(modem_open(&config, &modem) == ESP_OK);
        assert(modem_reconnect_pending(modem));
        assert(modem_command(modem, "AT", NULL, 100, NULL, 0, &result) == ESP_ERR_INVALID_STATE);
        assert(modem_sync_startup(modem) == ESP_ERR_TIMEOUT);
        assert(queries == 0);
        incoming = sms ? "\r\nOK\r\n> " : "\r\nOK\r\n";
        response_at = elapsed + 500000;
        assert(modem_sync_startup(modem) == ESP_OK);
        assert(queries == 1 && !notifications && editor_aborts == sms);
        assert(!modem_reconnect_pending(modem) && modem_maintenance_ready(modem));
        modem_close(modem);
    }
}

static void online_sampling_cases(void)
{
    modem_t modem = {.config = {.port = 1}};
    scenario = STATUS_SAMPLE;
    incoming = "";
    lte_status_t lte;
    gps_status_t gps;
    const char *data;
    bool gps_running;
    lte_registered = true;
    gps_sample_error = true;
    assert(sample_modem_status(&modem, &lte, &gps, &data, &gps_running));
    assert(!gps_running && gps == GPS_UNAVAILABLE && lte == LTE_HOME);
    gps_sample_late = true;
    lte_registered = false;
    assert(sample_modem_status(&modem, &lte, &gps, &data, &gps_running));
    assert(!gps_running && gps == GPS_UNAVAILABLE && lte == LTE_NOT_REGISTERED);
    assert(sample_modem_status(&modem, &lte, &gps, &data, &gps_running));
    assert(gps_running && gps == GPS_NO_FIX && lte == LTE_NOT_REGISTERED);
}

static void health_retry_cases(void)
{
    modem_lifecycle_t health = {.health_deadline = MODEM_BOOT_HEALTH_MS};
    marks = rollbacks = restarts = 0;
    boot_state = ESP_OTA_IMG_PENDING_VERIFY;
    read_failures = 1;
    modem_lifecycle_check_health(&health, 0, true);
    assert(!health.health_finished && health.health_errors == 1 && marks == 0);
    mark_error = ESP_FAIL;
    modem_lifecycle_check_health(&health, 1000, true);
    assert(!health.health_finished && marks == 1 && boot_state == ESP_OTA_IMG_PENDING_VERIFY);
    mark_error = ESP_OK;
    modem_lifecycle_check_health(&health, 2000, true);
    assert(health.health_finished && marks == 2 && boot_state == ESP_OTA_IMG_VALID);

    health = (modem_lifecycle_t){.health_deadline = MODEM_BOOT_HEALTH_MS};
    boot_state = ESP_OTA_IMG_PENDING_VERIFY;
    marks = 0;
    lose_mark_readback = true;
    modem_lifecycle_check_health(&health, 0, true);
    assert(!health.health_finished && marks == 1 && boot_state == ESP_OTA_IMG_VALID);
    // The metadata was written, but only the next verified read resolves it.
    modem_lifecycle_check_health(&health, 1000, true);
    assert(health.health_finished && marks == 1);

    health = (modem_lifecycle_t){.health_deadline = MODEM_BOOT_HEALTH_MS};
    boot_state = ESP_OTA_IMG_PENDING_VERIFY;
    marks = rollbacks = restarts = 0;
    rollback_error = ESP_FAIL;
    modem_lifecycle_check_health(&health, MODEM_BOOT_HEALTH_MS - 1, false);
    assert(modem_lifecycle_begin(&health, MODEM_BOOT_HEALTH_MS - 1));
    // Initialization completed healthy after the deadline, despite a pending retry.
    modem_lifecycle_result(&health, true, MODEM_BOOT_HEALTH_MS + 1);
    modem_lifecycle_check_health(&health, MODEM_BOOT_HEALTH_MS + 1, true);
    assert(!health.health_finished && rollbacks == 1 && !marks && !restarts);
    modem_lifecycle_check_health(&health, MODEM_BOOT_HEALTH_MS + 2, true);
    assert(rollbacks == 1);
    rollback_error = ESP_OK;
    modem_lifecycle_check_health(&health, MODEM_BOOT_HEALTH_MS + 1001, true);
    assert(health.health_finished && rollbacks == 2 && !marks &&
           restarts == 1 && boot_state == ESP_OTA_IMG_INVALID);

    health = (modem_lifecycle_t){.health_deadline = MODEM_BOOT_HEALTH_MS};
    boot_state = ESP_OTA_IMG_PENDING_VERIFY;
    fallback = false;
    for (int offset = 0; offset <= 4000; offset += 1000) {
        modem_lifecycle_check_health(&health, MODEM_BOOT_HEALTH_MS + offset, true);
    }
    assert(health.health_blocked && !health.health_finished && health.health_errors == 3);
    assert(rollbacks == 2 && !marks && restarts == 1);
    fallback = true;
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
    lifecycle_cases();
    health_retry_cases();
    reconnect_cases();
    online_sampling_cases();
    worker_handoff_case();
    puts("Modem lifecycle, startup and stale-final regressions, VALID/PENDING health, and private identity capture passed");
}
