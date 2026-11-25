#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HTTP_BUFFER 1024

enum {
    ESP_OK, ESP_FAIL, ESP_ERR_INVALID_SIZE, ESP_ERR_INVALID_RESPONSE, ESP_ERR_TIMEOUT,
};

typedef int esp_err_t;
typedef struct { bool up; } esp_netif_t;

typedef struct {
    const char *bytes;
    size_t length;
    size_t offset;
    size_t chunk;
    bool complete;
    bool error;
    esp_netif_t *drop_link;
} fixture_t;
typedef fixture_t *esp_http_client_handle_t;

static int64_t now;

static int64_t esp_timer_get_time(void)
{
    return now;
}

static bool cellular_ppp_is_uplink(esp_netif_t *netif)
{
    return netif->up;
}

static int esp_http_client_read(fixture_t *fixture, char *buffer, size_t limit)
{
    assert(limit >= fixture->chunk);
    if (fixture->error) {
        return -1;
    }
    size_t count = fixture->length - fixture->offset;
    if (count > fixture->chunk) {
        count = fixture->chunk;
    }
    memcpy(buffer, fixture->bytes + fixture->offset, count);
    fixture->offset += count;
    if (fixture->drop_link) {
        fixture->drop_link->up = false;
    }
    return count;
}

static bool esp_http_client_is_complete_data_received(fixture_t *fixture)
{
    return fixture->complete && fixture->offset == fixture->length;
}

#include "transfer_functions.inc"

static esp_err_t consume(void *context, const uint8_t *bytes, size_t count)
{
    char **destination = context;
    memcpy(*destination, bytes, count);
    *destination += count;
    return ESP_OK;
}

static esp_err_t run(fixture_t *fixture, esp_netif_t *netif, int64_t length, int64_t deadline,
                     char *output, size_t maximum, size_t exact)
{
    body_t body = {.maximum = maximum, .exact = exact, .consume = consume, .context = &output};
    fixture->offset = 0;
    uint8_t buffer[HTTP_BUFFER];
    return read_body(fixture, length, netif, deadline, &body, buffer);
}

int main(void)
{
    fixture_t fixture = {.bytes = "firmware", .length = 8, .chunk = 3, .complete = true};
    esp_netif_t netif = {.up = true};
    char output[8] = {0};
    assert(run(&fixture, &netif, 8, 10, output, 8, 8) == ESP_OK);
    assert(!memcmp(output, "firmware", 8));
    assert(run(&fixture, &netif, 0, 10, output, 8, 8) == ESP_OK);
    assert(run(&fixture, &netif, 9, 10, output, 8, 8) == ESP_ERR_INVALID_SIZE);
    assert(fixture.offset == 0);
    assert(run(&fixture, &netif, 7, 10, output, 8, 8) == ESP_ERR_INVALID_SIZE);
    assert(fixture.offset == 0);
    assert(run(&fixture, &netif, 0, 10, output, 6, 6) == ESP_ERR_INVALID_SIZE);

    fixture.length = 5;
    fixture.complete = false;
    assert(run(&fixture, &netif, 8, 10, output, 8, 8) == ESP_ERR_INVALID_RESPONSE);
    fixture.complete = true;
    assert(run(&fixture, &netif, 0, 10, output, 8, 8) == ESP_ERR_INVALID_SIZE);
    fixture.error = true;
    assert(run(&fixture, &netif, 8, 10, output, 8, 8) == ESP_FAIL);
    fixture.error = false;

    now = 10;
    assert(run(&fixture, &netif, 8, 10, output, 8, 8) == ESP_ERR_TIMEOUT);
    assert(fixture.offset == 0);
    fixture.drop_link = &netif;
    memset(output, 0, sizeof(output));
    assert(run(&fixture, &netif, 8, 20, output, 8, 8) == ESP_ERR_TIMEOUT);
    assert(output[0] == 0);
    assert(run(&fixture, &netif, 8, 20, output, 8, 8) == ESP_ERR_TIMEOUT);

    assert(redirect_status(301) && redirect_status(302) && redirect_status(303));
    assert(redirect_status(307) && redirect_status(308));
    assert(!redirect_status(200) && !redirect_status(206));
    assert(!redirect_status(304) && !redirect_status(401) && !redirect_status(404));
    puts("OTA streaming bounds, interruption and HTTP status fixtures passed");
}
