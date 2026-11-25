#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>

enum { ERR_OK, ERR_INPROGRESS, ERR_MEM, LWIP_DNS_ADDRTYPE_IPV4 };
enum { ESP_OK, ESP_FAIL, ESP_ERR_INVALID_ARG, ESP_ERR_TIMEOUT, ESP_ERR_NOT_FOUND };
using err_t = int;
using esp_err_t = int;
using TickType_t = unsigned;
struct StaticSemaphore_t { bool signaled; };
using SemaphoreHandle_t = StaticSemaphore_t *;
struct ip4_addr_t { unsigned addr; };
using ip_addr_t = ip4_addr_t;

#define IP_IS_V4(address) ((address) != nullptr)
#define ip_2_ip4(address) (address)
#define pdTRUE true
#define pdMS_TO_TICKS(milliseconds) (milliseconds)

static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    storage->signaled = false;
    return storage;
}

using dns_callback = void (*)(const char *, const ip_addr_t *, void *);
struct Request {
    const char *host;
    dns_callback complete;
    void *context;
};
static std::array<Request, 2> pending;
static unsigned queued;
static bool reject_dispatch, resolve_immediately;
static const ip_addr_t answer{42};

static void finish(unsigned index, const ip_addr_t *address)
{
    auto request = pending.at(index);
    request.complete(request.host, address, request.context);
}

static void xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(!semaphore->signaled);
    semaphore->signaled = true;
}

static bool xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    assert(ticks == 0 || ticks == 12000);
    bool signaled = semaphore->signaled;
    semaphore->signaled = false;
    return signaled;
}

static err_t dns_gethostbyname_addrtype(const char *host, ip_addr_t *address, dns_callback complete,
                                       void *context, unsigned type)
{
    assert(type == LWIP_DNS_ADDRTYPE_IPV4);
    if (resolve_immediately) {
        *address = answer;
        return ERR_OK;
    }
    pending.at(queued++) = {host, complete, context};
    return ERR_INPROGRESS;
}

static err_t tcpip_try_callback(void (*start)(void *), void *context)
{
    if (reject_dispatch) {
        return ERR_MEM;
    }
    start(context);
    return ERR_OK;
}

static char *ip4addr_ntoa_r(const ip4_addr_t *address, char *output, size_t size)
{
    assert(address->addr == answer.addr);
    return snprintf(output, size, "192.0.2.42") < static_cast<int>(size) ? output : nullptr;
}

#include "dns_source.inc"

int main()
{
    char address[16] = {};
    assert(cellular_resolve_ipv4("first.example", address, sizeof(address)) == ESP_ERR_TIMEOUT);
    assert(cellular_resolve_ipv4("second.example", address, sizeof(address)) == ESP_ERR_TIMEOUT);
    assert(cellular_resolve_ipv4("busy.example", address, sizeof(address)) == ESP_ERR_TIMEOUT);
    assert(queued == 2);
    assert(!strcmp(pending[0].host, "first.example") && !strcmp(pending[1].host, "second.example"));

    finish(1, &answer);
    finish(0, nullptr);
    queued = 0;
    // A late signal, including an address, cannot satisfy a new request.
    assert(cellular_resolve_ipv4("third.example", address, sizeof(address)) == ESP_ERR_TIMEOUT);
    assert(cellular_resolve_ipv4("fourth.example", address, sizeof(address)) == ESP_ERR_TIMEOUT);
    assert(queued == 2 && !address[0]);
    finish(0, &answer);
    finish(1, nullptr);

    reject_dispatch = true;
    assert(cellular_resolve_ipv4("queue-full.example", address, sizeof(address)) == ESP_FAIL);
    reject_dispatch = false;
    resolve_immediately = true;
    assert(cellular_resolve_ipv4("success.example", address, sizeof(address)) == ESP_OK);
    assert(!strcmp(address, "192.0.2.42"));
    puts("DNS slots reclaimed after two timeouts and late callbacks; stale signals and queue rejection covered");
}
