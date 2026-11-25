#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
typedef int uart_port_t;
typedef void *QueueHandle_t;
typedef struct { int type; } uart_event_t;
typedef struct {
    int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk;
} uart_config_t;

enum {
    ESP_OK, ESP_FAIL, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE,
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
static esp_err_t uart_flush_input(uart_port_t port);
static void vTaskDelay(unsigned ticks);

#include "modem_header.inc"
#include "modem_source.inc"

typedef enum { HEALTHY, STALE_PPP, BARE_OK, NO_ESCAPE, HANGUP_FAILED, FINAL_PROBE_FAILED, UART_FAULT } case_t;
static case_t scenario;
static int64_t elapsed, last_write;
static const char *incoming;
static unsigned queries, escapes, hangups, notifications;
static bool fault;

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
    if (!*incoming) {
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

static esp_err_t uart_flush_input(uart_port_t port)
{
    assert(port == 1);
    incoming = "";
    return ESP_OK;
}

static int uart_write_bytes(uart_port_t port, const char *data, size_t size)
{
    assert(port == 1 && !*incoming);
    if (size == 3 && !memcmp(data, "+++", 3)) {
        assert(++escapes == 1 && elapsed - last_write >= 1200000);
        if (scenario != NO_ESCAPE) {
            incoming = "\r\nOK\r\n";
        }
    } else if (size == 9 && !memcmp(data, "AT+CMGF?\r", 9)) {
        ++queries;
        if (queries == 2) {
            assert(escapes == 1 && elapsed - last_write >= 1200000);
        }
        if (scenario == HEALTHY || (escapes && scenario != BARE_OK &&
                                    !(scenario == FINAL_PROBE_FAILED && queries == 3))) {
            incoming = "\r\n+CMGF: 0\r\n\r\nOK\r\n";
        } else if (scenario == BARE_OK) {
            incoming = "\r\nOK\r\n";
        } else {
            incoming = "partial PPP data without a final";
        }
    } else if (size == 8 && !memcmp(data, "AT+CGSN\r", 8)) {
        incoming = "\r\n123456789012345\r\nOK\r\n";
    } else {
        assert(size == 4 && !memcmp(data, "ATH\r", 4));
        assert(escapes == 1 && queries == 2 && ++hangups == 1);
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

static void run_case(case_t test, esp_err_t expected, unsigned expected_queries,
                     unsigned expected_escapes, unsigned expected_hangups)
{
    modem_t modem = {.config = {.port = 1, .on_notification = notify}};
    scenario = test;
    elapsed = last_write = queries = escapes = hangups = notifications = 0;
    incoming = "";
    fault = test == UART_FAULT;
    assert(modem_sync_startup(&modem) == expected);
    assert(queries == expected_queries && escapes == expected_escapes && hangups == expected_hangups);
    assert(!notifications && modem.config.on_notification == notify);
}

int main(void)
{
    run_case(HEALTHY, ESP_OK, 1, 0, 0);
    run_case(STALE_PPP, ESP_OK, 3, 1, 1);
    run_case(BARE_OK, ESP_ERR_INVALID_STATE, 2, 1, 0);
    run_case(NO_ESCAPE, ESP_ERR_INVALID_STATE, 1, 1, 0);
    run_case(HANGUP_FAILED, ESP_ERR_INVALID_STATE, 2, 1, 1);
    run_case(FINAL_PROBE_FAILED, ESP_ERR_INVALID_STATE, 3, 1, 1);
    run_case(UART_FAULT, ESP_FAIL, 0, 0, 0);
    modem_t modem = {.config = {.port = 1, .on_notification = notify}};
    char identity[20];
    modem_result_t result;
    assert(modem_maintenance_ready(&modem));
    modem.waiting_for_final = true;
    assert(!modem_maintenance_ready(&modem));
    modem.waiting_for_final = false;
    assert(modem_command(&modem, "AT+CGSN", "+CGSN:", 1000, identity, sizeof(identity), &result) == ESP_OK);
    assert(!strcmp(identity, "123456789012345\n") && result.final == MODEM_FINAL_OK && !notifications);
    puts("Seven modem startup recovery cases and private solicited identity capture passed");
}
