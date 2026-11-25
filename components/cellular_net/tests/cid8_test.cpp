#include "../at_response.hpp"
#include "../modem_maintenance.hpp"

#include <array>
#include <cassert>
#include <charconv>
#include <cinttypes>
#include <memory>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
#include <openssl/sha.h>

using cellular_net::run_at;
using esp_modem::command_result;
using std::string_view;

using esp_err_t = int;
enum {
    ESP_OK, ESP_FAIL, ESP_ERR_INVALID_STATE, ESP_ERR_INVALID_RESPONSE, ESP_ERR_NVS_NOT_FOUND,
    ESP_ERR_INVALID_SIZE, NVS_READONLY, NVS_READWRITE,
};
using nvs_handle_t = unsigned;
static const char *TAG = "fixture";
static void log_message(const char *, const char *, ...) {}
#define ESP_LOGW log_message
#define ESP_LOGE log_message
#define ESP_LOGI log_message
static const char *esp_err_to_name(esp_err_t) { return "fixture"; }
static constexpr unsigned diagnostic_cid = 8;
static constexpr auto apn = "test.example";
using digest_t = std::array<uint8_t, 32>;

static std::optional<digest_t> journal;
static std::optional<digest_t> pending;
static bool fail_commit;

static esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "roadhog_ppp") == 0);
    *handle = mode;
    pending = journal;
    return ESP_OK;
}
static esp_err_t nvs_get_blob(nvs_handle_t, const char *key, void *value, size_t *length)
{
    assert(strcmp(key, "cid8_v1") == 0 && *length == sizeof(digest_t));
    if (!journal) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    memcpy(value, journal->data(), *length);
    return ESP_OK;
}
static esp_err_t nvs_set_blob(nvs_handle_t, const char *key, const void *value, size_t length)
{
    assert(strcmp(key, "cid8_v1") == 0 && length == sizeof(digest_t));
    pending.emplace();
    memcpy(pending->data(), value, length);
    return ESP_OK;
}
static esp_err_t nvs_erase_key(nvs_handle_t, const char *key)
{
    assert(strcmp(key, "cid8_v1") == 0);
    pending.reset();
    return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t)
{
    if (fail_commit) {
        return ESP_FAIL;
    }
    journal = pending;
    return ESP_OK;
}
static void nvs_close(nvs_handle_t) {}
static constexpr int PSA_SUCCESS = 0, PSA_ALG_SHA_256 = 1;
static int psa_crypto_init() { return PSA_SUCCESS; }
static int psa_hash_compute(int algorithm, const uint8_t *value, size_t length,
                            uint8_t *digest, size_t capacity, size_t *used)
{
    assert(algorithm == PSA_ALG_SHA_256 && capacity == 32);
    *used = 32;
    return SHA256(value, length, digest) ? PSA_SUCCESS : 1;
}

struct DCE {
    std::string imei = "123456789012345";
    std::string model = "SIMCOM_SIM7600NA-H";
    std::string profile;
    std::string secondary = "+CGDSCONT:\r\n";
    std::string malformed_primary;
    bool active = false;
    bool bare_ok = false;
    bool lost_define_final = false;
    bool fail_remove = false;
    unsigned definitions = 0, removals = 0, deactivations = 0;

    template<typename Callback>
    command_result command(const char *raw, Callback callback, uint32_t)
    {
        string_view cmd(raw);
        std::string response;
        if (cmd == "AT+CGDCONT?\r") {
            response = bare_ok ? "" : "+CGDCONT: 1,\"IP\",\"carrier\"\r\n";
            if (!profile.empty()) {
                response += "+CGDCONT: 8," + profile + "\r\n";
            }
            response += malformed_primary;
        } else if (cmd == "AT+CGDSCONT?\r") {
            response = secondary;
        } else if (cmd == "AT+CGACT?\r") {
            response = "+CGACT: 1,1\r\n+CGACT: 8," + std::to_string(active) + "\r\n";
        } else if (cmd == "AT+CGMM\r") {
            response = model + "\r\n";
        } else if (cmd == "AT+CGSN\r") {
            response = imei + "\r\n";
        } else if (cmd == "AT+CMGF?\r") {
            response = "+CMGF: 0\r\n";
        } else if (cmd == "AT+CGDCONT=8,\"IP\",\"test.example\"\r") {
            assert(journal && profile.empty() && !active);
            ++definitions;
            profile = "\"IP\",\"test.example\",\"0.0.0.0\",0,0,0,0";
            if (lost_define_final) {
                return command_result::TIMEOUT;
            }
        } else if (cmd == "AT+CGACT=0,8\r") {
            assert(active && !profile.empty() && journal);
            ++deactivations;
            active = false;
        } else if (cmd == "AT+CGDCONT=8\r") {
            assert(!active && !profile.empty());
            ++removals;
            if (fail_remove) {
                response = "ERROR\r\n";
                return callback(reinterpret_cast<uint8_t *>(response.data()), response.size());
            }
            profile.clear();
        } else {
            assert(false && "unexpected modem command");
        }
        response += "OK\r\n";
        return callback(reinterpret_cast<uint8_t *>(response.data()), response.size());
    }
};

template<typename Modem>
static command_result run_at(Modem &dce, const char *command, uint32_t timeout_ms)
{
    return run_at(dce, command, timeout_ms, [](string_view line) {
        return line == "OK" ? command_result::OK : command_result::TIMEOUT;
    });
}

#include "cid8.inc"

enum { MODEM_FINAL_NONE, MODEM_FINAL_OK, MODEM_FINAL_ERROR };
static constexpr size_t MODEM_LINE_CAPACITY = 384, MODEM_COMMAND_CAPACITY = 128;
static constexpr int ESP_ERR_TIMEOUT = 100;
struct modem_result_t { int final; char final_line[MODEM_LINE_CAPACITY]; };
struct modem_t { DCE &dce; bool idle = true; bool recover = true; bool closed = false; };
static int64_t now_us = 1;
static int64_t esp_timer_get_time() { return now_us; }
static uint32_t esp_random() { return 0x12abcdef; }
static bool modem_maintenance_ready(modem_t *modem) { return modem && modem->idle; }
static esp_err_t modem_poll(modem_t *modem, unsigned) { return modem->recover ? ESP_OK : ESP_FAIL; }
static void modem_close(modem_t *modem) { modem->closed = true; }
static bool cellular_apn_valid(const char *value) { return value && *value; }

static esp_err_t modem_command(modem_t *modem, const char *command, const char *prefix,
                              unsigned timeout, char *out, size_t capacity, modem_result_t *result)
{
    *result = {};
    *out = 0;
    if (!modem->idle) {
        return ESP_ERR_INVALID_STATE;
    }
    std::string wire = std::string(command) + "\r";
    auto code = modem->dce.command(wire.c_str(), [&](uint8_t *data, size_t length) {
        string_view remaining(reinterpret_cast<const char *>(data), length);
        while (!remaining.empty()) {
            const auto end = remaining.find("\r\n");
            assert(end != string_view::npos);
            auto line = remaining.substr(0, end);
            remaining.remove_prefix(end + 2);
            if (line == "OK" || line == "ERROR") {
                result->final = line == "OK" ? MODEM_FINAL_OK : MODEM_FINAL_ERROR;
                snprintf(result->final_line, sizeof(result->final_line), "%.*s",
                         static_cast<int>(line.size()), line.data());
                return command_result::OK;
            }
            if ((prefix && line.starts_with(prefix)) ||
                (wire == "AT+CGSN\r" && line.size() == 15)) {
                const size_t used = strlen(out);
                assert(used + line.size() + 2 <= capacity);
                snprintf(out + used, capacity - used, "%.*s\n", static_cast<int>(line.size()), line.data());
            }
        }
        return command_result::TIMEOUT;
    }, timeout);
    return code == command_result::OK ? ESP_OK : ESP_ERR_TIMEOUT;
}

#include "maintenance.inc"

static void test_maintenance()
{
    using cellular_net::maintenance_action;
    using cellular_net::parse_maintenance;
    assert(parse_maintenance("modem inspect") == maintenance_action::inspect);
    assert(parse_maintenance("modem cleanup 8") == maintenance_action::arm);
    assert(parse_maintenance("modem cleanup 7") == maintenance_action::invalid);
    assert(parse_maintenance("modem cleanup 8 confirm abcdefab ") == maintenance_action::invalid);
    assert(parse_maintenance("AT+CGDCONT=8") == maintenance_action::unrelated);
    assert(parse_maintenance("modem cleanup 8 confirm 12ABCDEF") == maintenance_action::invalid);

    journal.reset();
    fail_commit = false;
    DCE dce;
    dce.profile = "\"IP\",\"test.example\",\"0.0.0.0\",0,0";
    modem_t device{dce};
    modem_t *modem = &device;
    auto call = [&](const char *line) { return cellular_modem_maintenance(&modem, line, apn); };
    auto inspect_and_arm = [&] {
        assert(call("modem inspect"));
        assert(call("modem cleanup 8"));
    };
    const char *confirm = "modem cleanup 8 confirm 12abcdef";
    assert(call(confirm) && dce.removals == 0);
    device.idle = false;
    inspect_and_arm();
    assert(call(confirm) && dce.removals == 0);
    device.idle = true;

    dce.model = "different modem";
    inspect_and_arm();
    assert(call(confirm) && dce.removals == 0);
    dce.model = "SIMCOM_SIM7600NA-H";
    inspect_and_arm();
    dce.imei = "123456789012346";
    assert(call(confirm) && dce.removals == 0);

    inspect_and_arm();
    now_us += 61 * INT64_C(1000000);
    assert(call(confirm) && dce.removals == 0);
    inspect_and_arm();
    assert(call("modem cleanup 8 confirm 00000000"));
    assert(call(confirm) && dce.removals == 0);
    inspect_and_arm();
    assert(!call("ota check 2"));
    assert(call(confirm) && dce.removals == 0);

    inspect_and_arm();
    dce.active = true;
    assert(call(confirm) && dce.removals == 0);
    dce.active = false;
    inspect_and_arm();
    dce.secondary = "+CGDSCONT: 2,8,0,0\r\n";
    assert(call(confirm) && dce.removals == 0);
    dce.secondary = "+CGDSCONT:\r\n";
    inspect_and_arm();
    auto expected_profile = dce.profile;
    for (const char *profile : {"\"IP\",\"foreign.example\"", "\"IPV6\",\"test.example\"",
                                "\"IP\",\"test.example\",\"10.0.0.1\""}) {
        dce.profile = profile;
        inspect_and_arm();
        assert(call(confirm) && dce.removals == 0);
    }
    dce.profile = expected_profile;

    dce.fail_remove = true;
    inspect_and_arm();
    assert(call(confirm) && dce.removals == 1 && !dce.profile.empty() && modem == &device);
    assert(call(confirm) && dce.removals == 1);
    dce.fail_remove = false;
    inspect_and_arm();
    assert(call(confirm) && dce.removals == 2 && dce.profile.empty() && modem == &device);

    dce.profile = expected_profile;
    inspect_and_arm();
    device.recover = false;
    assert(call(confirm) && device.closed && modem == nullptr);
    puts("Console grammar, one-shot confirmation, busy UART, profile/identity guards, and cleanup recovery passed");
}

static bool blocked(DCE &dce, esp_err_t expected)
{
    auto previous = journal;
    bool owned = false;
    auto mutations = dce.definitions + dce.removals + dce.deactivations;
    return acquire_context(dce, apn, owned) == expected && !owned && previous == journal &&
           mutations == dce.definitions + dce.removals + dce.deactivations;
}

int main()
{
    DCE modem;
    bool owned = false;
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        assert(acquire_context(modem, apn, owned) == ESP_OK && owned && journal);
        modem.active = true;
        assert(remove_test_context(modem, apn, true) && !journal && modem.profile.empty());
        owned = false;
    }
    assert(modem.definitions == 2 && modem.deactivations == 2 && modem.removals == 2);

    modem.lost_define_final = true;
    assert(acquire_context(modem, apn, owned) == ESP_FAIL && owned && journal);
    // Reboot: no session flag survives. Only committed ownership and the
    // matching inactive context survive.
    auto stranded = modem;
    owned = false;
    modem.lost_define_final = false;
    assert(acquire_context(modem, apn, owned) == ESP_OK && owned && modem.definitions == 3);
    modem.fail_remove = true;
    assert(!remove_test_context(modem, apn, true) && journal && !modem.profile.empty());
    modem.fail_remove = false;
    assert(remove_test_context(modem, apn, true) && !journal);

    assert(blocked(stranded, ESP_ERR_INVALID_STATE)); // legacy or user-owned, even same APN
    assert(acquire_context(modem, apn, owned) == ESP_OK && journal);
    DCE wrong_modem = modem;
    wrong_modem.imei = "123456789012346";
    assert(blocked(wrong_modem, ESP_ERR_INVALID_STATE));
    DCE active = modem;
    active.active = true;
    assert(blocked(active, ESP_ERR_INVALID_STATE));
    assert(!remove_test_context(active, apn, false) && active.deactivations == modem.deactivations);
    DCE incompatible = modem;
    incompatible.profile = "\"IP\",\"other.example\",\"0.0.0.0\",0,0";
    assert(blocked(incompatible, ESP_ERR_INVALID_STATE));
    assert(!remove_test_context(incompatible, apn, true));
    incompatible.profile = "\"IP\",\"test.example\",\"0.0.0.0\",1,0";
    assert(blocked(incompatible, ESP_ERR_INVALID_STATE));
    DCE secondary = modem;
    secondary.secondary = "+CGDSCONT: 9,8,0,0\r\n";
    assert(blocked(secondary, ESP_ERR_INVALID_STATE));
    DCE malformed;
    malformed.malformed_primary = "+CGDCONT: invalid\r\n";
    assert(blocked(malformed, ESP_ERR_INVALID_RESPONSE));
    malformed.malformed_primary = "+CGDCONT: 8,\"IP\",\"test.example\"\r\n"
                                  "+CGDCONT: 8,\"IP\",\"test.example\"\r\n";
    assert(blocked(malformed, ESP_ERR_INVALID_RESPONSE));
    DCE empty;
    empty.bare_ok = true;
    assert(blocked(empty, ESP_ERR_INVALID_RESPONSE));
    DCE no_record;
    fail_commit = true;
    assert(blocked(no_record, ESP_FAIL));
    puts("CID 8 ownership, conflict handling, power-loss recovery, cleanup and repeated session cases passed");
    test_maintenance();
}
