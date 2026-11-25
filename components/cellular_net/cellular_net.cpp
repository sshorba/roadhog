#include "cellular_net.h"

#include <atomic>
#include <charconv>
#include <cinttypes>
#include <new>
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
#include "psa/crypto.h"
#include "nvs.h"
#include "esp_random.h"
#include "modem_maintenance.hpp"
extern "C" {
#include "modem.h"
}

using esp_modem::command_result;
using esp_modem::DCE;
using esp_modem::modem_mode;
using std::string_view;

static const char *TAG = "cellular_net";
static const char *const test_host = "connectivitycheck.gstatic.com";
// Keep the carrier's existing slots intact. If CID 8 is in use, skip the test.
static constexpr unsigned diagnostic_cid = 8;

using cellular_net::run_at;

template<typename Modem>
static command_result run_at(Modem &dce, const char *command, uint32_t timeout_ms)
{
    return run_at(dce, command, timeout_ms, [](string_view line) {
        return line == "OK" ? command_result::OK : command_result::TIMEOUT;
    });
}

static bool parse_number(string_view &fields, unsigned &value)
{
    size_t start = fields.find_first_not_of(" \t");
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

enum class context_kind { primary, secondary, activity };

struct context_query_t {
    const char *read;
    string_view prefix;
    context_kind kind;
};

static constexpr context_query_t primary_contexts{"AT+CGDCONT?\r", "+CGDCONT:", context_kind::primary};
static constexpr context_query_t secondary_contexts{"AT+CGDSCONT?\r", "+CGDSCONT:", context_kind::secondary};
static constexpr context_query_t active_contexts{"AT+CGACT?\r", "+CGACT:", context_kind::activity};

struct context_inventory_t {
    bool present = false;
    bool compatible = false;
    bool confirmed = false;
    uint32_t cid_mask = 0;
};

static bool comma(string_view &fields)
{
    const auto next = fields.find_first_not_of(" \t");
    if (next == string_view::npos || fields[next] != ',') {
        return false;
    }
    fields.remove_prefix(next + 1);
    return true;
}

static bool quoted_field(string_view &fields, string_view &value)
{
    const auto first = fields.find_first_not_of(" \t");
    if (first == string_view::npos || fields[first] != '"') {
        return false;
    }
    const auto last = fields.find('"', first + 1);
    if (last == string_view::npos) {
        return false;
    }
    value = fields.substr(first + 1, last - first - 1);
    fields.remove_prefix(last + 1);
    return true;
}

static bool compatible_primary(string_view fields, string_view apn)
{
    string_view type, configured_apn, address;
    if (!quoted_field(fields, type) || !comma(fields) || !quoted_field(fields, configured_apn) ||
        type != "IP" || configured_apn != apn) {
        return false;
    }
    if (fields.find_first_not_of(" \t") == string_view::npos) {
        return true;
    }
    if (!comma(fields) || !quoted_field(fields, address) ||
        (!address.empty() && address != "0.0.0.0")) {
        return false;
    }
    // Compression, IPv4 control and emergency settings must remain defaults.
    for (unsigned count = 0; fields.find_first_not_of(" \t") != string_view::npos; ++count) {
        unsigned value;
        if (count == 4 || !comma(fields) || !parse_number(fields, value) || value != 0) {
            return false;
        }
    }
    return true;
}

template<typename Modem>
static context_inventory_t query_context(Modem &dce, const context_query_t &query, string_view apn = {},
                                         uint8_t *profile_digest = nullptr)
{
    context_inventory_t inventory;
    bool empty = false, have_rows = false, malformed = false, seen_target = false;
    auto result = run_at(dce, query.read, 3000, [&](string_view line) {
        if (line == "OK") {
            // A bare OK can be the delayed final of an earlier command.
            return !malformed && (have_rows || empty) ? command_result::OK : command_result::FAIL;
        }
        if (!line.starts_with(query.prefix)) {
            return command_result::TIMEOUT;
        }
        line.remove_prefix(query.prefix.size());
        if (line.find_first_not_of(" \t") == string_view::npos) {
            malformed |= empty || have_rows;
            empty = true;
            return command_result::TIMEOUT;
        }

        unsigned cid = 0, value = 0;
        bool valid = parse_number(line, cid) && cid != 0 && comma(line);
        if (valid && query.kind == context_kind::activity) {
            valid = parse_number(line, value) && value <= 1 &&
                    line.find_first_not_of(" \t") == string_view::npos;
        } else if (valid && query.kind == context_kind::secondary) {
            unsigned parent = 0;
            valid = parse_number(line, parent) && parent != 0;
            for (unsigned count = 0; valid && line.find_first_not_of(" \t") != string_view::npos; ++count) {
                unsigned setting;
                valid = count < 2 && comma(line) && parse_number(line, setting);
            }
            // A secondary context tied to CID 8 also forbids changing it.
            inventory.present |= valid && parent == diagnostic_cid;
        } else if (valid) {
            auto definition = line;
            string_view type, configured_apn;
            valid = quoted_field(line, type) && !type.empty() && comma(line) &&
                    quoted_field(line, configured_apn);
            if (valid && cid == diagnostic_cid) {
                inventory.compatible = compatible_primary(definition, apn);
                if (inventory.compatible && profile_digest) {
                    size_t length = 0;
                    valid = psa_crypto_init() == PSA_SUCCESS &&
                            psa_hash_compute(PSA_ALG_SHA_256,
                                             reinterpret_cast<const uint8_t *>(definition.data()), definition.size(),
                                             profile_digest, 32, &length) == PSA_SUCCESS && length == 32;
                }
            }
        }
        have_rows = true;
        malformed |= !valid || empty;
        if (valid && cid >= 1 && cid <= 32 && (query.kind != context_kind::activity || value == 1)) {
            inventory.cid_mask |= UINT32_C(1) << (cid - 1);
        }
        if (valid && cid == diagnostic_cid) {
            malformed |= seen_target;
            seen_target = true;
            inventory.present |= query.kind != context_kind::activity || value == 1;
        }
        return command_result::TIMEOUT;
    });
    inventory.confirmed = result == command_result::OK;
    if (!inventory.confirmed) {
        ESP_LOGW(TAG, "CID %u inventory missing, malformed or rejected (%.*s)", diagnostic_cid,
                 static_cast<int>(query.prefix.size()), query.prefix.data());
    }
    return inventory;
}

template<typename Modem>
static bool query_value(Modem &dce, const char *command, string_view prefix, unsigned &value)
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
template<typename Modem>
static bool command_mode_ready(Modem &dce)
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

struct cid_owner_t {
    uint8_t digest[32];
};

template<typename Modem>
static bool identify_owner(Modem &dce, const char *apn, cid_owner_t &owner)
{
    char imei[16] = {};
    bool seen = false;
    auto result = run_at(dce, "AT+CGSN\r", 3000, [&](string_view line) {
        if (line == "OK") {
            return seen ? command_result::OK : command_result::FAIL;
        }
        if (line.starts_with("+CGSN:")) {
            line.remove_prefix(sizeof("+CGSN:") - 1);
            const auto first = line.find_first_not_of(" \t");
            line = first == string_view::npos ? string_view{} : line.substr(first);
        }
        if (line.size() == 15 && line.find_first_not_of("0123456789") == string_view::npos) {
            if (seen) {
                return command_result::FAIL;
            }
            memcpy(imei, line.data(), line.size());
            seen = true;
        }
        return command_result::TIMEOUT;
    });
    // Only the digest is persisted. Never put modem identifiers or APNs in logs.
    char identity[140];
    int length = snprintf(identity, sizeof(identity), "cid8/ip/v1:%s:%s", imei, apn);
    size_t digest_size = 0;
    return result == command_result::OK && length > 0 && length < static_cast<int>(sizeof(identity)) &&
           psa_crypto_init() == PSA_SUCCESS &&
           psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t *>(identity), length,
                            owner.digest, sizeof(owner.digest), &digest_size) == PSA_SUCCESS &&
           digest_size == sizeof(owner.digest);
}

static esp_err_t load_owner(cid_owner_t &owner)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("roadhog_ppp", NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error;
    }
    size_t length = sizeof(owner);
    error = nvs_get_blob(handle, "cid8_v1", &owner, &length);
    nvs_close(handle);
    return error == ESP_OK && length != sizeof(owner) ? ESP_ERR_INVALID_SIZE : error;
}

static esp_err_t save_owner(const cid_owner_t *owner)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("roadhog_ppp", NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = owner ? nvs_set_blob(handle, "cid8_v1", owner, sizeof(*owner))
                  : nvs_erase_key(handle, "cid8_v1");
    if (!owner && error == ESP_ERR_NVS_NOT_FOUND) {
        error = ESP_OK;
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

static bool owner_matches(const cid_owner_t &expected, const cid_owner_t &saved)
{
    return memcmp(expected.digest, saved.digest, sizeof(expected.digest)) == 0;
}

// Called only with command mode verified and exclusive UART ownership.
static esp_err_t acquire_context(DCE &dce, const char *apn, bool &owned)
{
    auto primary = query_context(dce, primary_contexts, apn);
    if (!primary.confirmed) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    auto secondary = query_context(dce, secondary_contexts);
    if (!secondary.confirmed) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    auto active = query_context(dce, active_contexts);
    if (!active.confirmed) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (active.present || secondary.present) {
        ESP_LOGW(TAG, "CID %u %s conflict; leaving all PDP contexts untouched", diagnostic_cid,
                 active.present ? "active" : "secondary");
        return ESP_ERR_INVALID_STATE;
    }
    cid_owner_t expected{}, saved{};
    if (!identify_owner(dce, apn, expected)) {
        ESP_LOGW(TAG, "Cannot verify diagnostic PDP modem identity");
        return ESP_ERR_INVALID_RESPONSE;
    }
    esp_err_t error = load_owner(saved);
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Cannot read diagnostic PDP ownership: %s", esp_err_to_name(error));
        return error;
    }
    if (primary.present) {
        if (!primary.compatible || error != ESP_OK || !owner_matches(expected, saved)) {
            ESP_LOGW(TAG, "CID %u has %s primary definition; leaving it untouched", diagnostic_cid,
                     primary.compatible ? "an unowned compatible" : "an incompatible");
            return ESP_ERR_INVALID_STATE;
        }
        owned = true;
        ESP_LOGI(TAG, "Reusing verified owned inactive CID %u", diagnostic_cid);
        return ESP_OK;
    }

    // Record intent before transmission, including when the final is lost.
    // A prior record is replaceable only after the entire CID is proven free.
    error = save_owner(&expected);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Cannot persist diagnostic PDP ownership: %s", esp_err_to_name(error));
        return error;
    }
    owned = true;
    char command[128];
    snprintf(command, sizeof(command), "AT+CGDCONT=%u,\"IP\",\"%s\"\r", diagnostic_cid, apn);
    if (run_at(dce, command, 3000) != command_result::OK) {
        ESP_LOGE(TAG, "Temporary PDP definition failed");
        return ESP_FAIL;
    }
    primary = query_context(dce, primary_contexts, apn);
    if (!primary.confirmed || !primary.present || !primary.compatible) {
        ESP_LOGE(TAG, "Temporary PDP definition could not be verified");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static bool remove_test_context(DCE &dce, const char *apn, bool dial_attempted)
{
    cid_owner_t expected{}, saved{};
    if (!identify_owner(dce, apn, expected) || load_owner(saved) != ESP_OK ||
        !owner_matches(expected, saved)) {
        ESP_LOGW(TAG, "Diagnostic PDP cleanup deferred: ownership cannot be verified");
        return false;
    }
    auto primary = query_context(dce, primary_contexts, apn);
    auto secondary = query_context(dce, secondary_contexts);
    auto active = query_context(dce, active_contexts);
    if (!primary.confirmed || !secondary.confirmed || !active.confirmed || secondary.present ||
        (primary.present && !primary.compatible) || (active.present && (!primary.present || !dial_attempted))) {
        ESP_LOGW(TAG, "Diagnostic PDP cleanup deferred: CID %u inventory conflict", diagnostic_cid);
        return false;
    }
    char command[32];
    if (active.present) {
        snprintf(command, sizeof(command), "AT+CGACT=0,%u\r", diagnostic_cid);
        run_at(dce, command, 5000);
        if (!command_mode_ready(dce)) {
            return false;
        }
        active = query_context(dce, active_contexts);
        if (!active.confirmed || active.present) {
            return false;
        }
    }
    if (primary.present) {
        snprintf(command, sizeof(command), "AT+CGDCONT=%u\r", diagnostic_cid);
        run_at(dce, command, 3000);
        if (!command_mode_ready(dce)) {
            return false;
        }
        primary = query_context(dce, primary_contexts, apn);
        if (!primary.confirmed || primary.present) {
            return false;
        }
    }
    if (save_owner(nullptr) != ESP_OK) {
        ESP_LOGE(TAG, "Could not clear diagnostic PDP ownership record");
        return false;
    }
    ESP_LOGI(TAG, "Temporary PDP context removed and verified");
    return true;
}

#ifdef CONFIG_ROADHOG_MODEM_MAINTENANCE_BENCH
// Adapt the existing AT owner to the inventory parser. This adapter has
// no dial, reset or arbitrary AT capability.
class maintenance_modem {
public:
    explicit maintenance_modem(modem_t *modem)
        : modem_(modem), response_(new (std::nothrow) char[cellular_net::at_response_capacity]) {}

    explicit operator bool() const { return bool(response_); }

    template<typename Callback>
    command_result command(const char *wire, Callback callback, uint32_t timeout)
    {
        string_view command(wire);
        const char *prefix = nullptr;
        if (command == "AT+CGDCONT?\r") {
            prefix = "+CGDCONT:";
        } else if (command == "AT+CGDSCONT?\r") {
            prefix = "+CGDSCONT:";
        } else if (command == "AT+CGACT?\r") {
            prefix = "+CGACT:";
        } else if (command == "AT+CGSN\r") {
            prefix = "+CGSN:";
        } else if (command == "AT+CGMM\r") {
            prefix = "SIMCOM_SIM7600";
        } else if (command == "AT+CMGF?\r") {
            prefix = "+CMGF:";
        } else if (command != "AT+CGDCONT=8\r") {
            return command_result::FAIL;
        }
        char text[MODEM_COMMAND_CAPACITY];
        snprintf(text, sizeof(text), "%.*s", static_cast<int>(command.size() - 1), command.data());
        modem_result_t result;
        const auto error = modem_command(modem_, text, prefix, timeout, response_.get(),
                                         cellular_net::at_response_capacity - MODEM_LINE_CAPACITY - 1,
                                         &result);
        if (error != ESP_OK || result.final == MODEM_FINAL_NONE) {
            return error == ESP_ERR_TIMEOUT ? command_result::TIMEOUT : command_result::FAIL;
        }
        size_t used = strlen(response_.get());
        snprintf(response_.get() + used, cellular_net::at_response_capacity - used,
                 "%s\n", result.final_line);
        return callback(reinterpret_cast<uint8_t *>(response_.get()), strlen(response_.get()));
    }

private:
    modem_t *modem_;
    std::unique_ptr<char[]> response_;
};

struct legacy_snapshot {
    cid_owner_t identity{};
    uint8_t profile[32]{};
    context_inventory_t primary{}, secondary{}, active{};
    bool eligible = false;
};

static bool same_snapshot(const legacy_snapshot &a, const legacy_snapshot &b)
{
    return a.eligible && b.eligible && owner_matches(a.identity, b.identity) &&
           memcmp(a.profile, b.profile, sizeof(a.profile)) == 0 &&
           a.primary.cid_mask == b.primary.cid_mask &&
           a.secondary.cid_mask == b.secondary.cid_mask &&
           a.active.cid_mask == b.active.cid_mask;
}

template<typename Modem>
static legacy_snapshot inspect_legacy(Modem &modem, const char *apn)
{
    legacy_snapshot snapshot;
    bool model = false;
    auto result = run_at(modem, "AT+CGMM\r", 3000, [&](string_view line) {
        if (line.starts_with("SIMCOM_SIM7600")) {
            model = true;
        }
        return line == "OK" && model ? command_result::OK : command_result::TIMEOUT;
    });
    if (result != command_result::OK || !identify_owner(modem, apn ? apn : "", snapshot.identity)) {
        ESP_LOGW(TAG, "SIM7600 identity could not be verified; no PDP operation performed");
        return snapshot;
    }
    snapshot.primary = query_context(modem, primary_contexts, apn ? apn : "", snapshot.profile);
    snapshot.secondary = query_context(modem, secondary_contexts);
    snapshot.active = query_context(modem, active_contexts);
    if (!snapshot.primary.confirmed || !snapshot.secondary.confirmed || !snapshot.active.confirmed) {
        ESP_LOGW(TAG, "PDP inventory incomplete; cleanup blocked");
        return snapshot;
    }
    ESP_LOGI(TAG, "SIM7600 verified; modem/APN identifiers withheld");
    ESP_LOGI(TAG, "PDP CIDs 1–32 masks (bit 0 = CID 1): primary=%08" PRIx32
             " secondary=%08" PRIx32 " active=%08" PRIx32,
             snapshot.primary.cid_mask, snapshot.secondary.cid_mask, snapshot.active.cid_mask);
    ESP_LOGI(TAG, "CID 8: primary=%s compatible=%s active=%s secondary-or-dependent=%s",
             snapshot.primary.present ? "present" : "absent",
             snapshot.primary.compatible ? "yes" : "no",
             snapshot.active.present ? "yes" : "no",
             snapshot.secondary.present ? "yes" : "no");

    cid_owner_t saved{};
    const auto journal = load_owner(saved);
    snapshot.eligible = apn && cellular_apn_valid(apn) && snapshot.primary.present &&
                        snapshot.primary.compatible && !snapshot.secondary.present &&
                        !snapshot.active.present && journal == ESP_ERR_NVS_NOT_FOUND;
    if (!snapshot.eligible) {
        ESP_LOGW(TAG, "Legacy CID 8 cleanup blocked by profile, activity, dependency or ownership record");
    }
    return snapshot;
}

template<typename Modem>
static bool cleanup_legacy(Modem &modem, const char *apn, const legacy_snapshot &before)
{
    // One scoped command, including after timeout or a lost final. Readback
    // decides whether a later operator review is needed. Never retry a write.
    const auto command = run_at(modem, "AT+CGDCONT=8\r", 3000);
    if (!command_mode_ready(modem)) {
        ESP_LOGE(TAG, "CID 8 cleanup not verified: AT command mode unavailable");
        return false;
    }
    auto primary = query_context(modem, primary_contexts, apn);
    auto secondary = query_context(modem, secondary_contexts);
    auto active = query_context(modem, active_contexts);
    constexpr uint32_t bit8 = UINT32_C(1) << 7;
    if (primary.confirmed && secondary.confirmed && active.confirmed) {
        ESP_LOGI(TAG, "PDP after cleanup: primary=%08" PRIx32
                 " secondary=%08" PRIx32 " active=%08" PRIx32,
                 primary.cid_mask, secondary.cid_mask, active.cid_mask);
    }
    bool unchanged = primary.confirmed && secondary.confirmed && active.confirmed &&
                     primary.cid_mask == (before.primary.cid_mask & ~bit8) &&
                     secondary.cid_mask == before.secondary.cid_mask &&
                     active.cid_mask == before.active.cid_mask;
    if (unchanged && !primary.present && !secondary.present && !active.present) {
        ESP_LOGI(TAG, "Legacy CID 8 removal verified; other observed PDP masks unchanged");
        return true;
    }
    ESP_LOGE(TAG, "CID 8 cleanup incomplete or inventory changed (command acknowledged=%s); no retry",
             command == command_result::OK ? "yes" : "no");
    return false;
}
#endif

bool cellular_modem_maintenance(modem_t **modem, const char *line, const char *apn)
{
#ifdef CONFIG_ROADHOG_MODEM_MAINTENANCE_BENCH
    using cellular_net::maintenance_action;
    const auto action = cellular_net::parse_maintenance(line);
    static constexpr int64_t window_us = 60 * INT64_C(1000000);
    static legacy_snapshot inspected;
    static int64_t inspect_until;
    static cellular_net::cleanup_confirmation confirmation;

    if (action == maintenance_action::unrelated || action == maintenance_action::invalid) {
        inspect_until = confirmation.expires = 0;
        if (action == maintenance_action::invalid) {
            ESP_LOGW(TAG, "Unknown modem maintenance command");
        }
        return action == maintenance_action::invalid;
    }
    if (!modem || !modem_maintenance_ready(*modem)) {
        inspect_until = confirmation.expires = 0;
        ESP_LOGW(TAG, "Modem UART busy; maintenance refused");
        return true;
    }
    const auto now = esp_timer_get_time();
    if (action == maintenance_action::arm) {
        confirmation.expires = 0;
        if (inspect_until > now && inspected.eligible) {
            snprintf(confirmation.token, sizeof(confirmation.token), "%08" PRIx32, esp_random());
            confirmation.expires = now + window_us;
            ESP_LOGW(TAG, "Confirm historical ownership, then enter: modem cleanup 8 confirm %s",
                     confirmation.token);
        } else {
            ESP_LOGW(TAG, "Run modem inspect and verify the legacy context before arming cleanup");
        }
        inspect_until = 0;
        return true;
    }

    if (action == maintenance_action::confirm && !confirmation.consume(line, now)) {
        inspect_until = 0;
        ESP_LOGW(TAG, "Cleanup confirmation invalid or expired; no PDP change");
        return true;
    }
    confirmation.expires = inspect_until = 0;
    maintenance_modem adapter(*modem);
    if (!adapter) {
        ESP_LOGE(TAG, "No memory for modem maintenance");
        return true;
    }
    auto live = inspect_legacy(adapter, apn);
    if (action == maintenance_action::inspect) {
        inspected = live;
        inspect_until = esp_timer_get_time() + window_us;
    } else if (same_snapshot(inspected, live)) {
        (void)cleanup_legacy(adapter, apn, live);
    } else {
        ESP_LOGW(TAG, "CID 8 snapshot changed or became unsafe; no PDP change");
    }

    // Drain late finals and confirm the same owner can resume AT. Do not
    // reset, escape or send SMS as part of maintenance.
    if (modem_poll(*modem, 250) != ESP_OK || !command_mode_ready(adapter)) {
        inspect_until = 0;
        ESP_LOGE(TAG, "Normal modem command mode could not be verified; stopped");
        modem_close(*modem);
        *modem = nullptr;
    }
    return true;
#else
    (void)modem;
    (void)line;
    (void)apn;
    return false;
#endif
}

bool cellular_ppp_is_uplink(esp_netif_t *netif)
{
    return esp_netif_is_netif_up(netif) && esp_netif_get_default_netif() == netif;
}

static esp_err_t wait_for_ppp(esp_netif_t *netif)
{
    int64_t deadline = esp_timer_get_time() + 45000000;
    while (esp_timer_get_time() < deadline) {
        esp_netif_ip_info_t ip;
        if (cellular_ppp_is_uplink(netif) && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr) {
            ESP_LOGI(TAG, "PPP IPv4 acquired: " IPSTR, IP2STR(&ip.ip));
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGE(TAG, "PPP did not become the IPv4 uplink within 45 seconds");
    return ESP_ERR_TIMEOUT;
}

struct dns_lookup_t {
    // Both the waiting task and the lwIP callback hold the slot until they
    // finish. The callback may outlive the task's deadline.
    std::atomic<unsigned> references{};
    StaticSemaphore_t storage;
    SemaphoreHandle_t done;
    ip4_addr_t address;
    char host[254];
};

static void dns_complete(const char *, const ip_addr_t *address, void *context)
{
    auto &lookup = *static_cast<dns_lookup_t *>(context);
    if (address && IP_IS_V4(address)) {
        lookup.address = *ip_2_ip4(address);
    }
    xSemaphoreGive(lookup.done);
    lookup.references.fetch_sub(1, std::memory_order_acq_rel);
}

static void start_dns(void *context)
{
    ip_addr_t address;
    auto &lookup = *static_cast<dns_lookup_t *>(context);
    err_t result = dns_gethostbyname_addrtype(lookup.host, &address, dns_complete, context,
                                             LWIP_DNS_ADDRTYPE_IPV4);
    if (result != ERR_INPROGRESS) {
        dns_complete(lookup.host, result == ERR_OK ? &address : nullptr, context);
    }
}

esp_err_t cellular_resolve_ipv4(const char *host, char *address, size_t size)
{
    static dns_lookup_t slots[2];
    if (strlen(host) >= sizeof(slots[0].host)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (auto &lookup : slots) {
        unsigned unused = 0;
        if (!lookup.references.compare_exchange_strong(unused, 2, std::memory_order_acq_rel)) {
            continue;
        }
        if (!lookup.done) {
            lookup.done = xSemaphoreCreateBinaryStatic(&lookup.storage);
        }
        // A callback after the previous caller's timeout may have left a signal.
        xSemaphoreTake(lookup.done, 0);
        strcpy(lookup.host, host);
        lookup.address = {};
        esp_err_t result = ESP_ERR_TIMEOUT;
        if (tcpip_try_callback(start_dns, &lookup) != ERR_OK) {
            lookup.references.store(0, std::memory_order_release);
            return ESP_FAIL;
        }
        if (xSemaphoreTake(lookup.done, pdMS_TO_TICKS(12000)) == pdTRUE) {
            result = lookup.address.addr && ip4addr_ntoa_r(&lookup.address, address, size)
                     ? ESP_OK : ESP_ERR_NOT_FOUND;
        }
        lookup.references.fetch_sub(1, std::memory_order_acq_rel);
        return result;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t prepare_tls(esp_netif_t *netif)
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

    esp_err_t error;
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
    if (!cellular_ppp_is_uplink(netif)) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static esp_err_t check_https(esp_netif_t *netif)
{
    char address[16];
    esp_err_t error = cellular_resolve_ipv4(test_host, address, sizeof(address));
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "DNS lookup failed: %s", esp_err_to_name(error));
        return error;
    }
    ESP_LOGI(TAG, "DNS resolved %s", test_host);

    struct ifreq interface = {};
    error = esp_netif_get_netif_impl_name(netif, interface.ifr_name);
    if (error != ESP_OK) {
        return error;
    }
    char url[sizeof("https://255.255.255.255/generate_204")];
    snprintf(url, sizeof(url), "https://%s/generate_204", address);
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
            if (!cellular_ppp_is_uplink(netif)) {
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
    bool context_owned = false;
    bool dial_attempted = false;
    bool ppp_transition_attempted = false;
};

static esp_err_t test_session(DCE &dce, esp_netif_t *netif, const char *apn, session_state_t &state,
                              cellular_operation_t operation, void *context)
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

    esp_err_t error = acquire_context(dce, apn, state.context_owned);
    if (error != ESP_OK) {
        return error;
    }
    char command[128];
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

    error = wait_for_ppp(netif);
    if (error == ESP_OK) {
        error = prepare_tls(netif);
    }
    return error == ESP_OK ? (operation ? operation(netif, context) : check_https(netif)) : error;
}

esp_err_t cellular_net_session(const cellular_net_config_t *config, bool *at_ready,
                               cellular_operation_t operation, void *context)
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
        error = test_session(*dce, netif.get(), config->apn, state, operation, context);
    } catch (const std::exception &) {
        ESP_LOGE(TAG, "esp_modem failed during the diagnostic; attempting cleanup");
    }

    ESP_LOGI(TAG, "Closing LTE diagnostic session");
    try {
        *at_ready = recover_at(*dce, state.dial_attempted, state.ppp_transition_attempted);
        if (*at_ready && state.context_owned &&
            !remove_test_context(*dce, config->apn, state.dial_attempted)) {
            ESP_LOGE(TAG, "Diagnostic PDP cleanup incomplete; retaining ownership record");
            error = ESP_FAIL;
            *at_ready = command_mode_ready(*dce);
        }
    } catch (const std::exception &) {
        *at_ready = false;
        ESP_LOGE(TAG, "esp_modem failed during cleanup");
    }
    if (!*at_ready) {
        ESP_LOGE(TAG, "Could not verify command mode");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Returning UART with modem in command mode");
    return error;
}

esp_err_t cellular_net_test(const cellular_net_config_t *config, bool *at_ready)
{
    return cellular_net_session(config, at_ready, nullptr, nullptr);
}
#endif
