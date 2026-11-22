#include "cellular_net.h"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>

#include "sdkconfig.h"

bool cellular_apn_valid(const char *apn)
{
    const char allowed[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-";
    size_t length = strnlen(apn, 101);
    return length > 0 && length <= 100 && strspn(apn, allowed) == length;
}

#ifdef CONFIG_ROADHOG_LTE_DIAGNOSTIC

#include "at_response.hpp"
#include "cxx_include/esp_modem_api.hpp"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_modem_config.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"

using esp_modem::command_result;
using esp_modem::DCE;
using esp_modem::modem_mode;
using std::string_view;

static const char *TAG = "cellular_net";
static const char *const test_host = "connectivitycheck.gstatic.com";
// Keep the carrier's existing slots intact. If CID 8 is in use, skip the test.
static constexpr unsigned diagnostic_cid = 8;

using cellular_net::run_at;

static command_result run_at(DCE &dce, const char *command, uint32_t timeout_ms)
{
    return run_at(dce, command, timeout_ms, [](string_view line) {
        return line == "OK" ? command_result::OK : command_result::TIMEOUT;
    });
}

static bool parse_number(string_view &fields, unsigned &value)
{
    size_t start = fields.find_first_not_of(' ');
    if (start == string_view::npos) {
        return false;
    }
    fields.remove_prefix(start);
    auto end = fields.data() + fields.size();
    auto result = std::from_chars(fields.data(), end, value);
    if (result.ec != std::errc{}) {
        return false;
    }
    fields.remove_prefix(result.ptr - fields.data());
    return true;
}

static bool query_context(DCE &dce, const char *command, string_view prefix, bool &present)
{
    present = false;
    bool valid = true;
    auto result = run_at(dce, command, 3000, [&](string_view line) {
        if (line == "OK") {
            return valid ? command_result::OK : command_result::FAIL;
        }
        if (line.starts_with(prefix)) {
            line.remove_prefix(prefix.size());
            unsigned cid;
            if (!parse_number(line, cid) || !line.starts_with(',')) {
                valid = false;
            } else if (cid == diagnostic_cid) {
                present = true;
                if (prefix == "+CGACT:") {
                    line.remove_prefix(1);
                    unsigned state;
                    if (!parse_number(line, state) || !line.empty() || state > 1) {
                        valid = false;
                    } else {
                        present = state == 1;
                    }
                }
            }
        }
        return command_result::TIMEOUT;
    });
    return result == command_result::OK;
}

static bool query_value(DCE &dce, const char *command, string_view prefix, unsigned &value)
{
    bool seen = false;
    auto result = run_at(dce, command, 2000, [&](string_view line) {
        if (line == "OK") {
            return seen ? command_result::OK : command_result::TIMEOUT;
        }
        if (line.starts_with(prefix)) {
            line.remove_prefix(prefix.size());
            seen = parse_number(line, value) && line.empty() && value <= 1;
        }
        return command_result::TIMEOUT;
    });
    return result == command_result::OK;
}

// A response carrying the expected payload prevents a delayed bare OK from
// an earlier command being mistaken for restored command mode.
static bool command_mode_ready(DCE &dce)
{
    unsigned sms_format;
    return query_value(dce, "AT+CMGF?\r", "+CMGF:", sms_format);
}

static bool recover_at(DCE &dce, bool dial_attempted, bool ppp_started)
{
    if (ppp_started && !dce.set_mode(modem_mode::COMMAND_MODE)) {
        ESP_LOGW(TAG, "esp_modem did not confirm command mode after PPP stop");
    }
    // The public C++ transition updates the DTE without sending another dial.
    // Restore the text callback even if the modem-side transition failed.
    dce.set_mode(modem_mode::RESUME_COMMAND_MODE);
    if (command_mode_ready(dce)) {
        return true;
    }
    if (dial_attempted) {
        vTaskDelay(pdMS_TO_TICKS(1200));
        run_at(dce, "+++", 2500);
        vTaskDelay(pdMS_TO_TICKS(1200));
    }
    return command_mode_ready(dce);
}

static bool remove_test_context(DCE &dce)
{
    bool active;
    if (!query_context(dce, "AT+CGACT?\r", "+CGACT:", active)) {
        return false;
    }
    char command[32];
    if (active) {
        snprintf(command, sizeof(command), "AT+CGACT=0,%u\r", diagnostic_cid);
        run_at(dce, command, 5000);
        if (!command_mode_ready(dce) ||
            !query_context(dce, "AT+CGACT?\r", "+CGACT:", active) || active) {
            return false;
        }
    }
    bool defined;
    if (!query_context(dce, "AT+CGDCONT?\r", "+CGDCONT:", defined)) {
        return false;
    }
    if (defined) {
        snprintf(command, sizeof(command), "AT+CGDCONT=%u\r", diagnostic_cid);
        run_at(dce, command, 3000);
        if (!command_mode_ready(dce) ||
            !query_context(dce, "AT+CGDCONT?\r", "+CGDCONT:", defined) || defined) {
            return false;
        }
    }
    ESP_LOGI(TAG, "Temporary PDP context removed and verified");
    return true;
}

static bool ppp_is_uplink(esp_netif_t *netif)
{
    return esp_netif_is_netif_up(netif) && esp_netif_get_default_netif() == netif;
}

static esp_err_t wait_for_ppp(esp_netif_t *netif)
{
    int64_t deadline = esp_timer_get_time() + 45000000;
    while (esp_timer_get_time() < deadline) {
        esp_netif_ip_info_t ip;
        if (ppp_is_uplink(netif) && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr) {
            ESP_LOGI(TAG, "PPP IPv4 acquired: " IPSTR, IP2STR(&ip.ip));
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGE(TAG, "PPP did not become the IPv4 uplink within 45 seconds");
    return ESP_ERR_TIMEOUT;
}

struct dns_lookup_t {
    SemaphoreHandle_t done;
    ip4_addr_t address;
};

static void dns_complete(const char *, const ip_addr_t *address, void *context)
{
    auto &lookup = *static_cast<dns_lookup_t *>(context);
    if (address && IP_IS_V4(address)) {
        lookup.address = *ip_2_ip4(address);
    }
    xSemaphoreGive(lookup.done);
}

static void start_dns(void *context)
{
    ip_addr_t address;
    err_t result = dns_gethostbyname_addrtype(test_host, &address, dns_complete, context,
                                             LWIP_DNS_ADDRTYPE_IPV4);
    if (result != ERR_INPROGRESS) {
        dns_complete(test_host, result == ERR_OK ? &address : nullptr, context);
    }
}

static esp_err_t resolve_test_host(ip4_addr_t &address)
{
    // One lookup per boot. lwIP may deliver the callback after our deadline.
    static StaticSemaphore_t done_storage;
    static dns_lookup_t lookup = {xSemaphoreCreateBinaryStatic(&done_storage), {}};
    if (tcpip_try_callback(start_dns, &lookup) != ERR_OK) {
        return ESP_FAIL;
    }
    if (xSemaphoreTake(lookup.done, pdMS_TO_TICKS(12000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    address = lookup.address;
    return address.addr ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t check_https(esp_netif_t *netif)
{
    esp_netif_dns_info_t dns = {};
    bool have_dns = esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
                    dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr;
    if (!have_dns &&
        (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns) != ESP_OK ||
         dns.ip.type != ESP_IPADDR_TYPE_V4 || !dns.ip.u_addr.ip4.addr)) {
        ESP_LOGE(TAG, "PPP did not supply an IPv4 DNS server");
        return ESP_ERR_INVALID_RESPONSE;
    }

    ip4_addr_t address;
    esp_err_t error = resolve_test_host(address);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "DNS lookup failed: %s", esp_err_to_name(error));
        return error;
    }
    ESP_LOGI(TAG, "DNS resolved %s", test_host);

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.cloudflare.com");
    error = esp_netif_sntp_init(&sntp);
    if (error == ESP_OK) {
        error = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
        esp_netif_sntp_deinit();
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Clock synchronization for TLS failed: %s", esp_err_to_name(error));
        return error;
    }
    if (!ppp_is_uplink(netif)) {
        return ESP_ERR_INVALID_STATE;
    }

    struct ifreq interface = {};
    error = esp_netif_get_netif_impl_name(netif, interface.ifr_name);
    if (error != ESP_OK) {
        return error;
    }
    char url[sizeof("https://255.255.255.255/generate_204")];
    snprintf(url, sizeof(url), "https://" IPSTR "/generate_204", IP2STR(&address));
    esp_http_client_config_t http = {};
    http.url = url;
    http.method = HTTP_METHOD_HEAD;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.common_name = test_host;  // Preserve hostname verification and SNI with a numeric URL.
    http.timeout_ms = 250;
    http.disable_auto_redirect = true;
    http.is_async = true;
    http.if_name = &interface;
    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    error = esp_http_client_set_header(client, "Host", test_host);
    if (error == ESP_OK) {
        int64_t deadline = esp_timer_get_time() + 10000000;
        do {
            if (!ppp_is_uplink(netif)) {
                error = ESP_ERR_INVALID_STATE;
                break;
            }
            error = esp_http_client_perform(client);
            if (esp_timer_get_time() >= deadline) {
                error = ESP_ERR_TIMEOUT;
                break;
            }
            if (error == ESP_ERR_HTTP_EAGAIN) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        } while (error == ESP_ERR_HTTP_EAGAIN);
    }
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

struct session_state_t {
    bool context_write_attempted = false;
    bool dial_attempted = false;
    bool ppp_transition_attempted = false;
};

static esp_err_t test_session(DCE &dce, esp_netif_t *netif, const char *apn, session_state_t &state)
{
    if (!command_mode_ready(dce)) {
        return ESP_ERR_INVALID_STATE;
    }
    unsigned attached;
    if (!query_value(dce, "AT+CGATT?\r", "+CGATT:", attached) || !attached) {
        ESP_LOGW(TAG, "Packet attachment unavailable; diagnostic skipped");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Packet attachment confirmed");

    bool used;
    if (!query_context(dce, "AT+CGDCONT?\r", "+CGDCONT:", used) || used ||
        !query_context(dce, "AT+CGDSCONT?\r", "+CGDSCONT:", used) || used ||
        !query_context(dce, "AT+CGACT?\r", "+CGACT:", used) || used) {
        ESP_LOGW(TAG, "Cannot confirm diagnostic CID %u is unused; diagnostic skipped", diagnostic_cid);
        return ESP_ERR_INVALID_STATE;
    }
    char command[128];
    snprintf(command, sizeof(command), "AT+CGDCONT=%u,\"IP\",\"%s\"\r", diagnostic_cid, apn);
    // An error or timeout can follow a modem-side change. Track the attempt
    // before sending, so every later exit verifies cleanup.
    state.context_write_attempted = true;
    if (run_at(dce, command, 3000) != command_result::OK) {
        ESP_LOGE(TAG, "Temporary PDP definition failed");
        return ESP_FAIL;
    }

    snprintf(command, sizeof(command), "AT+CGDATA=\"PPP\",%u\r", diagnostic_cid);
    ESP_LOGI(TAG, "Starting SIM7600 PPP negotiation");
    state.dial_attempted = true;
    auto dial = run_at(dce, command, 10000, [](string_view line) {
        if (line == "CONNECT" || line.starts_with("CONNECT ")) {
            return command_result::OK;
        }
        return line == "NO CARRIER" || line == "OK" ? command_result::FAIL : command_result::TIMEOUT;
    });
    if (dial != command_result::OK) {
        ESP_LOGE(TAG, "PPP dial failed: %s",
                 dial == command_result::TIMEOUT ? "CONNECT timeout" : "modem rejected dial");
        return dial == command_result::TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    state.ppp_transition_attempted = true;
    if (!dce.set_mode(modem_mode::RESUME_DATA_MODE)) {
        ESP_LOGE(TAG, "Could not start PPP on the connected UART");
        return ESP_FAIL;
    }

    esp_err_t error = wait_for_ppp(netif);
    return error == ESP_OK ? check_https(netif) : error;
}

esp_err_t cellular_net_test(const cellular_net_config_t *config, bool *at_ready)
{
    *at_ready = true;
    esp_netif_config_t net_config = ESP_NETIF_DEFAULT_PPP();
    std::unique_ptr<esp_netif_t, decltype(&esp_netif_destroy)> netif(esp_netif_new(&net_config),
                                                                    esp_netif_destroy);
    if (!netif) {
        return ESP_ERR_NO_MEM;
    }

    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.dte_buffer_size = cellular_net::at_response_capacity;
    dte_config.uart_config.rx_buffer_size = 8192;
    dte_config.uart_config.port_num = config->port;
    dte_config.uart_config.tx_io_num = config->tx_pin;
    dte_config.uart_config.rx_io_num = config->rx_pin;
    dte_config.uart_config.baud_rate = config->baud_rate;
    dte_config.uart_config.rts_io_num = UART_PIN_NO_CHANGE;
    dte_config.uart_config.cts_io_num = UART_PIN_NO_CHANGE;
    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(config->apn);
    auto dte = esp_modem::create_uart_dte(&dte_config);
    if (!dte) {
        return ESP_FAIL;
    }
    auto dce = esp_modem::create_SIM7600_dce(&dce_config, std::move(dte), netif.get());
    if (!dce) {
        return ESP_FAIL;
    }

    *at_ready = false;
    esp_err_t error = ESP_FAIL;
    session_state_t state;
    try {
        error = test_session(*dce, netif.get(), config->apn, state);
    } catch (const std::exception &) {
        ESP_LOGE(TAG, "esp_modem failed during the diagnostic; attempting cleanup");
    }

    ESP_LOGI(TAG, "Closing LTE diagnostic session");
    try {
        *at_ready = recover_at(*dce, state.dial_attempted, state.ppp_transition_attempted) &&
                    (!state.context_write_attempted || remove_test_context(*dce));
    } catch (const std::exception &) {
        ESP_LOGE(TAG, "esp_modem failed during cleanup");
    }
    if (!*at_ready) {
        ESP_LOGE(TAG, "Could not verify command mode or PDP cleanup");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Returning UART with modem in command mode");
    return error;
}
#endif
