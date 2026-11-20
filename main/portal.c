#include "portal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

static const char *TAG = "portal";
static char portal_url[sizeof("http://255.255.255.255/")];
static char portal_host[sizeof("255.255.255.255")];
static struct sockaddr_in dns_address;
static int dns_socket;
extern const char portal_html_start[] asm("_binary_index_html_start");

static esp_err_t landing_page(httpd_req_t *request)
{
    char host[sizeof("255.255.255.255:80")];
    esp_err_t host_result = httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host));
    if (host_result == ESP_OK && httpd_req_get_hdr_value_len(request, "Host") > 3 &&
        strcmp(host + strlen(host) - 3, ":80") == 0) {
        host[strlen(host) - 3] = '\0';
    }

    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    if ((host_result == ESP_ERR_NOT_FOUND || (host_result == ESP_OK && strcmp(host, portal_host) == 0)) &&
        (strcmp(request->uri, "/") == 0 || strncmp(request->uri, "/?", 2) == 0)) {
        return httpd_resp_sendstr(request, portal_html_start);
    }

    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", portal_url);
    return httpd_resp_sendstr(request, "Open the Road Hog setup portal.");
}

enum { DNS_HEADER = 12, DNS_MAX_QUERY = 512, DNS_A_RECORD = 16 };

static size_t dns_reply(uint8_t packet[DNS_MAX_QUERY + DNS_A_RECORD], size_t length)
{
    // Answer a standard query with one uncompressed question. Skip any EDNS
    // records so the reply contains only the question and optional A record.
    if (length < DNS_HEADER || (packet[2] & 0xf8) || packet[4] != 0 || packet[5] != 1) {
        return 0;
    }
    size_t offset = DNS_HEADER;
    while (offset < length && packet[offset] != 0) {
        unsigned label_length = packet[offset++];
        if (label_length > 63 || label_length > length - offset || offset + label_length > DNS_HEADER + 254) {
            return 0;
        }
        offset += label_length;
    }
    if (offset + 5 > length) {
        return 0;
    }
    offset++;
    bool ipv4 = packet[offset] == 0 && packet[offset + 1] == 1 &&
                packet[offset + 2] == 0 && packet[offset + 3] == 1;
    offset += 4;
    packet[2] = 0x84 | (packet[2] & 1); // Response, authoritative, preserve RD.
    packet[3] = 0;
    memset(packet + 6, 0, 6);
    if (ipv4) {
        static const uint8_t answer[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4};
        packet[7] = 1;
        memcpy(packet + offset, answer, sizeof(answer));
        offset += sizeof(answer);
        memcpy(packet + offset, &dns_address.sin_addr.s_addr, 4);
        offset += 4;
    }
    return offset;
}

static void dns_task(void *context)
{
    (void)context;
    uint8_t packet[DNS_MAX_QUERY + DNS_A_RECORD];
    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        ssize_t received = recvfrom(dns_socket, packet, DNS_MAX_QUERY, 0,
                                    (struct sockaddr *)&peer, &peer_length);
        if (received < 0) {
            ESP_LOGE(TAG, "Captive DNS receive failed");
            close(dns_socket);
            vTaskDelete(NULL);
            return;
        }
        size_t length = dns_reply(packet, received);
        if (length) {
            sendto(dns_socket, packet, length, 0, (struct sockaddr *)&peer, peer_length);
        }
    }
}

static esp_err_t start_services(void)
{
    httpd_handle_t http;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    esp_err_t error = httpd_start(&http, &config);
    if (error != ESP_OK) {
        return error;
    }
    const httpd_uri_t route = {.uri = "/*", .method = HTTP_GET, .handler = landing_page};
    error = httpd_register_uri_handler(http, &route);
    if (error == ESP_OK) {
        dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (dns_socket < 0) {
            error = ESP_FAIL;
        } else if (bind(dns_socket, (struct sockaddr *)&dns_address, sizeof(dns_address)) != 0 ||
                   xTaskCreate(dns_task, "portal_dns", 3072, NULL, 3, NULL) != pdPASS) {
            close(dns_socket);
            error = ESP_FAIL;
        }
    }
    if (error != ESP_OK) {
        httpd_stop(http);
    }
    return error;
}

static esp_err_t start_first_ap(void)
{
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "AP MAC");
    wifi_config_t wifi = {.ap = {.channel = 1, .max_connection = 4, .authmode = WIFI_AUTH_OPEN}};
    wifi.ap.ssid_len = snprintf((char *)wifi.ap.ssid, sizeof(wifi.ap.ssid),
                               "ROADHOG-%02X%02X", mac[4], mac[5]);

    static bool driver_initialized;
    if (!driver_initialized) {
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi initialization");
        driver_initialized = true;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "Wi-Fi AP mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi), TAG, "Wi-Fi AP setup");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi AP start");

    esp_err_t error = start_services();
    if (error != ESP_OK) {
        esp_wifi_stop();
        return error;
    }
    ESP_LOGI(TAG, "%s ready at %s", (char *)wifi.ap.ssid, portal_url);
    return ESP_OK;
}

// Only this controller starts and stops Wi-Fi. DNS and HTTP stay allocated
// across toggles, and the AP keeps its local address and DHCP configuration.
static SemaphoreHandle_t ap_lock;
static bool cellular_known;
static bool cellular_registered;
static bool ap_initialized;
static bool ap_running;
static int64_t idle_since;
static const int64_t idle_timeout = 5LL * 60 * 1000000;

static esp_err_t set_ap(bool enabled)
{
    if (enabled == ap_running) {
        return ESP_OK;
    }
    esp_err_t error;
    if (enabled) {
        error = ap_initialized ? esp_wifi_start() : start_first_ap();
    } else {
        error = esp_wifi_stop();
    }
    if (error == ESP_OK) {
        ap_initialized = true;
        ap_running = enabled;
        idle_since = 0;
        ESP_LOGI(TAG, "Setup AP %s", enabled ? "enabled" : "disabled");
    }
    return error;
}

static void ap_policy_task(void *context)
{
    (void)context;
    for (;;) {
        xSemaphoreTake(ap_lock, portMAX_DELAY);
        if (cellular_known && !cellular_registered && !ap_running) {
            esp_err_t error = set_ap(true);
            if (error != ESP_OK) {
                ESP_LOGW(TAG, "Could not enable fallback AP: %s", esp_err_to_name(error));
            }
        } else if (ap_running && cellular_registered) {
            wifi_sta_list_t stations;
            if (esp_wifi_ap_get_sta_list(&stations) == ESP_OK && stations.num == 0) {
                int64_t now = esp_timer_get_time();
                if (!idle_since) {
                    idle_since = now;
                } else if (now - idle_since >= idle_timeout) {
                    if (set_ap(false) == ESP_OK) {
                        ESP_LOGI(TAG, "Setup AP idle timeout");
                    }
                }
            } else {
                idle_since = 0;
            }
        }
        xSemaphoreGive(ap_lock);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t portal_init(void)
{
    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "NVS initialization");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Network initialization");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Event loop initialization");

    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    esp_netif_ip_info_t ip;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(ap, &ip), TAG, "AP address");
    snprintf(portal_host, sizeof(portal_host), IPSTR, IP2STR(&ip.ip));
    snprintf(portal_url, sizeof(portal_url), "http://%s/", portal_host);
    dns_address = (struct sockaddr_in){
        .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = ip.ip.addr,
    };

    ap_lock = xSemaphoreCreateMutex();
    if (!ap_lock) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(ap_policy_task, "portal_policy", 4096, NULL, 4, NULL) != pdPASS) {
        vSemaphoreDelete(ap_lock);
        ap_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void portal_set_cellular(bool registered)
{
    if (!ap_lock) {
        return;
    }
    xSemaphoreTake(ap_lock, portMAX_DELAY);
    if (!cellular_known || cellular_registered != registered) {
        cellular_known = true;
        cellular_registered = registered;
        idle_since = 0;
    }
    xSemaphoreGive(ap_lock);
}

esp_err_t portal_request(bool enabled)
{
    if (!ap_lock) {
        return ESP_FAIL;
    }
    xSemaphoreTake(ap_lock, portMAX_DELAY);
    esp_err_t error;
    if (!enabled && (!cellular_known || !cellular_registered)) {
        error = ESP_ERR_INVALID_STATE;
    } else {
        error = set_ap(enabled);
        if (error == ESP_OK && enabled) {
            idle_since = 0;
        }
    }
    xSemaphoreGive(ap_lock);
    return error;
}
