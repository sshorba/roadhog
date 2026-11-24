#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
typedef void *httpd_handle_t;
typedef void *esp_event_base_t;
typedef int httpd_err_code_t;
enum { ESP_OK, ESP_FAIL };
enum { HTTPD_403_FORBIDDEN = 403, HTTPD_404_NOT_FOUND = 404 };
enum { PROVISION_FACTORY = 1 };
enum { HTTP_SERVER_EVENT_ERROR, HTTP_SERVER_EVENT_STOP, HTTP_SERVER_EVENT_DISCONNECTED };

typedef struct {
    const char *uri, *host, *origin;
    const char *location;
    bool authenticated, send_failure;
    int status;
} httpd_req_t;

static const char *TAG = "portal";
static const char portal_host[] = "192.168.4.1";
static const char portal_url[] = "http://192.168.4.1/";
static const char portal_html_start[] = "<html></html>";
static struct sockaddr_in dns_address;
static struct sockaddr_storage fixture_address;
static socklen_t fixture_length;
static bool fixture_socket_error;
static unsigned begins, sends, failures, accepts, parser_errors, stops, closes, posts;

static void log_event(const char *tag, const char *format, ...)
{
    assert(!strcmp(tag, "portal"));
    begins += strstr(format, "GET begin") != NULL;
    sends += strstr(format, "GET sent") != NULL;
    failures += strstr(format, "GET failed") != NULL;
    accepts += strstr(format, "connection accepted") != NULL;
    parser_errors += strstr(format, "generated error response") != NULL;
    stops += strstr(format, "server stopped") != NULL;
    closes += strstr(format, "connection closed") != NULL;
}
#define ESP_LOGI log_event
#define ESP_LOGW log_event

static const char *esp_err_to_name(esp_err_t error) { return error ? "fixture failure" : "OK"; }
static unsigned uxTaskGetStackHighWaterMark(void *task) { assert(!task); return 4096; }
static int provisioning_state(void) { return PROVISION_FACTORY; }
static int httpd_req_to_sockfd(httpd_req_t *request) { assert(request); return 5; }

static int fixture_getsockname(int fd, struct sockaddr *out, socklen_t *length)
{
    assert(fd == 5);
    if (fixture_socket_error) {
        errno = EBADF;
        return -1;
    }
    socklen_t copied = *length < fixture_length ? *length : fixture_length;
    memcpy(out, &fixture_address, copied);
    *length = copied;
    return 0;
}
#define getsockname fixture_getsockname

static const char *header(httpd_req_t *req, const char *key)
{
    return !strcmp(key, "Host") ? req->host : req->origin;
}
static size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *key)
{
    const char *value = header(req, key);
    return value ? strlen(value) : 0;
}
static esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *key, char *out, size_t size)
{
    const char *value = header(req, key);
    if (!value || strlen(value) >= size) return ESP_FAIL;
    strcpy(out, value);
    return ESP_OK;
}
static void httpd_resp_set_hdr(httpd_req_t *req, const char *key, const char *value)
{
    if (!strcmp(key, "Location")) req->location = value;
}
static void httpd_resp_set_type(httpd_req_t *req, const char *type) { assert(req && type); }
static void httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    assert(sscanf(status, "%d", &req->status) == 1);
}
static esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *body)
{
    assert(body);
    if (req->send_failure) return ESP_FAIL;
    if (!req->status) req->status = 200;
    return ESP_OK;
}
static esp_err_t httpd_resp_send_err(httpd_req_t *req, int status, const char *body)
{
    req->status = status;
    return httpd_resp_sendstr(req, body);
}
static bool portal_authenticated(httpd_req_t *req) { return req->authenticated; }
static esp_err_t status_response(httpd_req_t *req) { return httpd_resp_sendstr(req, "private status"); }
static esp_err_t portal_auth_post(httpd_req_t *req)
{
    ++posts;
    return httpd_resp_sendstr(req, "auth handler");
}

#include "handlers.inc"
#include "events.inc"

static void ipv4(const char *address)
{
    struct sockaddr_in ip = {.sin_family = AF_INET};
    assert(inet_pton(AF_INET, address, &ip.sin_addr) == 1);
    memcpy(&fixture_address, &ip, sizeof(ip));
    fixture_length = sizeof(ip);
}

#if CONFIG_LWIP_IPV6
static void ipv6(const char *address)
{
    struct sockaddr_in6 ip = {.sin6_family = AF_INET6};
    assert(inet_pton(AF_INET6, address, &ip.sin6_addr) == 1);
    memcpy(&fixture_address, &ip, sizeof(ip));
    fixture_length = sizeof(ip);
}
#endif

static httpd_req_t get(const char *uri, const char *host, bool auth, int status)
{
    httpd_req_t req = {.uri = uri, .host = host, .authenticated = auth};
    assert(landing_page(&req) == ESP_OK);
    assert(req.status == status);
    return req;
}

int main(void)
{
    assert(inet_pton(AF_INET, portal_host, &dns_address.sin_addr) == 1);
    ipv4(portal_host);
    assert(http_open(NULL, 5) == ESP_OK && accepts == 1);
    // Accepted connection without a handler can be distinguished from a
    // handled send failure and from a successful redirect.
    assert(!begins && !sends && !failures);
    int event_data = 5;
    http_event(NULL, NULL, HTTP_SERVER_EVENT_ERROR, &event_data);
    http_event(NULL, NULL, HTTP_SERVER_EVENT_DISCONNECTED, &event_data);
    assert(parser_errors == 1 && closes == 1 && !begins);
    get("/", portal_host, false, 200);
    get("/?x=1", "192.168.4.1:80", false, 200);
    assert(!strcmp(get("/generate_204", "other.example", false, 302).location, portal_url));
    assert(!get("/", portal_host, false, 200).location);
    get("/api/status", portal_host, false, 401);
    get("/api/status", portal_host, true, 200);
    get("/api/status", "other.example", true, 403);
    get("/api/unknown", portal_host, true, 404);

    fixture_socket_error = true;
    assert(!get("/", portal_host, false, 403).location);
    fixture_socket_error = false;
    ipv4("192.168.5.1");
    get("/", portal_host, false, 403);
    get("/api/status", portal_host, true, 403);

#if CONFIG_LWIP_IPV6
    ipv6("::ffff:192.168.4.1");
    get("/", portal_host, false, 200);
    get("/api/status", portal_host, false, 401);
    get("/api/status", portal_host, true, 200);
    ipv6("::ffff:192.168.5.1");
    get("/", portal_host, false, 403);
    ipv6("fe80::1");
    get("/", portal_host, false, 403);
    ipv6("::ffff:192.168.4.1");
#else
    ipv4(portal_host);
#endif

    httpd_req_t post = {.host = portal_host, .origin = "http://192.168.4.1"};
    assert(auth_post(&post) == ESP_OK && posts == 1);
    post.origin = "http://other.example";
    assert(auth_post(&post) == ESP_OK && post.status == 403 && posts == 1);
    fixture_socket_error = true;
    post.origin = "http://192.168.4.1";
    assert(auth_post(&post) == ESP_OK && post.status == 403 && posts == 1);
    fixture_socket_error = false;
    httpd_req_t failed = {.uri = "/", .host = portal_host, .send_failure = true};
    assert(landing_page(&failed) == ESP_FAIL && failures == 1 && begins == sends + failures);
    http_event(NULL, NULL, HTTP_SERVER_EVENT_STOP, NULL);
    assert(stops == 1);
    printf("Portal listener, routing and diagnostic fixtures passed (IPv6=%d)\n", CONFIG_LWIP_IPV6);
}
