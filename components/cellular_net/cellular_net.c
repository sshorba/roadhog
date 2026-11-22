#include "cellular_net.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_modem_api.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

bool cellular_apn_valid(const char *apn)
{
    const char allowed[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-";
    size_t length = strnlen(apn, 101);
    return length > 0 && length <= 100 && strspn(apn, allowed) == length;
}

#ifdef CONFIG_ROADHOG_LTE_DIAGNOSTIC
static const char *TAG = "cellular_net";
#define TEST_HOST "connectivitycheck.gstatic.com"

static esp_err_t wait_for_ppp(esp_netif_t *netif)
{
    int64_t deadline = esp_timer_get_time() + 45000000;
    do {
        esp_netif_ip_info_t ip;
        if (esp_netif_is_netif_up(netif) &&
            esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr &&
            esp_netif_get_default_netif() == netif) {
            ESP_LOGI(TAG, "PPP IPv4 acquired: " IPSTR, IP2STR(&ip.ip));
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    } while (esp_timer_get_time() < deadline);
    ESP_LOGE(TAG, "PPP did not become the IPv4 uplink within 45 seconds");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t check_https(esp_netif_t *netif)
{
    esp_netif_dns_info_t dns = {0};
    bool have_dns = esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
                    dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr;
    if (!have_dns &&
        (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns) != ESP_OK ||
         dns.ip.type != ESP_IPADDR_TYPE_V4 || !dns.ip.u_addr.ip4.addr)) {
        ESP_LOGE(TAG, "PPP did not supply an IPv4 DNS server");
        return ESP_ERR_INVALID_RESPONSE;
    }
    struct addrinfo *addresses;
    const struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    int dns_error = getaddrinfo(TEST_HOST, NULL, &hints, &addresses);
    if (dns_error != 0) {
        ESP_LOGE(TAG, "DNS lookup failed (%d)", dns_error);
        return ESP_FAIL;
    }
    freeaddrinfo(addresses);
    ESP_LOGI(TAG, "DNS resolved " TEST_HOST);

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.cloudflare.com");
    esp_err_t error = esp_netif_sntp_init(&sntp);
    if (error == ESP_OK) {
        error = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
        esp_netif_sntp_deinit();
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Clock synchronization for TLS failed: %s", esp_err_to_name(error));
        return error;
    }

    struct ifreq interface = {0};
    error = esp_netif_get_netif_impl_name(netif, interface.ifr_name);
    if (error != ESP_OK) {
        return error;
    }
    esp_http_client_config_t http = {
        .url = "https://" TEST_HOST "/generate_204",
        .method = HTTP_METHOD_HEAD,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .disable_auto_redirect = true,
        .if_name = &interface,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    error = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "HTTPS transport or TLS verification failed: %s", esp_err_to_name(error));
        return error;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTPS returned HTTP %d", status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "HTTPS HEAD succeeded (HTTP %d, certificate verified)", status);
    return ESP_OK;
}

static bool recover_at(esp_modem_dce_t *dce, bool dial_attempted, bool ppp_started)
{
    if (ppp_started) {
        esp_err_t error = esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
        if (error == ESP_OK) {
            ESP_LOGI(TAG, "PPP stopped and command mode restored");
        } else {
            ESP_LOGW(TAG, "PPP shutdown did not confirm command mode; checking AT");
        }
    }
    if (esp_modem_sync(dce) != ESP_OK) {
        // TX must stay idle around the escape. esp_modem has already stopped PPP.
        vTaskDelay(pdMS_TO_TICKS(1200));
        esp_modem_at_raw(dce, "+++", NULL, "OK", "ERROR", 2500);
        vTaskDelay(pdMS_TO_TICKS(1200));
        if (esp_modem_sync(dce) != ESP_OK) {
            return false;
        }
    }
    if (dial_attempted) {
        // End a data call that may still be suspended after an escape.
        esp_err_t error = esp_modem_at_raw(dce, "ATH\r", NULL, "OK", "ERROR", 3000);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "Hangup did not return OK: %s", esp_err_to_name(error));
        }
    }
    return esp_modem_sync(dce) == ESP_OK;
}

esp_err_t cellular_net_test(const cellular_net_config_t *config, bool *at_ready)
{
    // Until the DCE is opened, nothing here has changed the physical modem.
    *at_ready = true;
    esp_netif_config_t net_config = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *netif = esp_netif_new(&net_config);
    if (!netif) {
        return ESP_ERR_NO_MEM;
    }
    esp_modem_dte_config_t dte = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte.uart_config.port_num = config->port;
    dte.uart_config.tx_io_num = config->tx_pin;
    dte.uart_config.rx_io_num = config->rx_pin;
    dte.uart_config.baud_rate = config->baud_rate;
    dte.uart_config.rts_io_num = UART_PIN_NO_CHANGE;
    dte.uart_config.cts_io_num = UART_PIN_NO_CHANGE;
    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(config->apn);
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte, &dce_config, netif);
    if (!dce) {
        esp_netif_destroy(netif);
        return ESP_FAIL;
    }

    esp_err_t error = esp_modem_sync(dce);
    bool dial_attempted = false;
    bool ppp_started = false;
    if (error == ESP_OK) {
        // Explicit attach is bounded even if the carrier stops responding.
        error = esp_modem_at_raw(dce, "AT+CGATT=1\r", NULL, "OK", "ERROR", 15000);
        ESP_LOGI(TAG, "Packet attach: %s", esp_err_to_name(error));
    }
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "Starting SIM7600 PPP negotiation");
        dial_attempted = true;
        error = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
        ppp_started = error == ESP_OK;
        if (!ppp_started) {
            ESP_LOGE(TAG, "PPP dial failed; check APN and SIM data provisioning");
        }
    }
    if (ppp_started) {
        error = wait_for_ppp(netif);
        if (error == ESP_OK) {
            error = check_https(netif);
        }
    }

    ESP_LOGI(TAG, "Closing LTE diagnostic session");
    *at_ready = recover_at(dce, dial_attempted, ppp_started);
    if (*at_ready && dial_attempted &&
        esp_modem_at_raw(dce, config->restore_pdp, NULL, "OK", "ERROR", 3000) != ESP_OK) {
        ESP_LOGE(TAG, "Could not restore the original PDP profile");
        if (error == ESP_OK) {
            error = ESP_FAIL;
        }
    }
    esp_modem_destroy(dce);
    esp_netif_destroy(netif);
    if (!*at_ready) {
        ESP_LOGE(TAG, "Modem did not confirm AT readiness; power-cycle modem and ESP32");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "PPP interface released, modem answered AT");
    return error;
}
#endif
