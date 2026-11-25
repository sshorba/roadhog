/* Host adapters for the production installer and pinned SDK staging functions. */
#include <assert.h>
#include <curl/curl.h>
#include <net/if.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/queue.h>
#include <time.h>

#include "ota_manifest.h"

static char update_report[160];

static void fixture_log(const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (!strncmp(format, "Update stopped", 14) || !strncmp(format, "Could not select", 16)) {
        vsnprintf(update_report, sizeof(update_report), format, args);
    } else if (getenv("OTA_TEST_VERBOSE")) {
        fprintf(stderr, "%s: ", tag);
        vfprintf(stderr, format, args);
        fputc('\n', stderr);
    }
    va_end(args);
}

#define ESP_LOGI fixture_log
#define ESP_LOGW ESP_LOGI
#define ESP_LOGD ESP_LOGI
#define ESP_LOGE ESP_LOGI
#define CONFIG_ROADHOG_OTA_RELEASE_BASE "https://github.com/sshorba/roadhog/releases/download"
#define CONFIG_ROADHOG_OTA_RELEASE_HOSTS "github.com,release-assets.githubusercontent.com,wrong.example"
#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1
#define FIXTURE_APN "fixture.carrier"
#define MIN(a, b) ((a) < (b) ? (a) : (b))

typedef int esp_err_t;
enum {
    ESP_OK, ESP_FAIL, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_INVALID_SIZE,
    ESP_ERR_INVALID_RESPONSE, ESP_ERR_INVALID_VERSION, ESP_ERR_INVALID_CRC, ESP_ERR_TIMEOUT,
    ESP_ERR_NO_MEM, ESP_ERR_NOT_FOUND, ESP_ERR_OTA_PARTITION_CONFLICT,
    ESP_ERR_OTA_ROLLBACK_INVALID_STATE, ESP_ERR_OTA_ALREADY_IN_PROGRESS, ESP_ERR_OTA_VALIDATE_FAILED,
    ESP_ERR_FLASH_OP_FAIL,
};

static const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_ERR_FLASH_OP_FAIL ? "ESP_ERR_FLASH_OP_FAIL" : "fixture status";
}

typedef int esp_partition_subtype_t;
typedef unsigned esp_ota_handle_t;
typedef int esp_ota_img_states_t;

enum {
    ESP_PARTITION_TYPE_APP, ESP_PARTITION_TYPE_DATA, ESP_PARTITION_TYPE_BOOTLOADER,
    ESP_PARTITION_TYPE_PARTITION_TABLE, ESP_PARTITION_SUBTYPE_APP_OTA_0,
    ESP_PARTITION_SUBTYPE_APP_OTA_1, ESP_PARTITION_SUBTYPE_DATA_NVS,
    ESP_PARTITION_SUBTYPE_DATA_PHY, ESP_PARTITION_SUBTYPE_DATA_OTA,
    ESP_OTA_IMG_VALID, ESP_OTA_IMG_PENDING_VERIFY,
    ESP_IMAGE_APPLICATION, ESP_IMAGE_HEADER_MAGIC = 0xe9, ESP_CHIP_ID_ESP32S3 = 9,
    ESP_IMAGE_FLASH_SIZE_16MB = 4, ESP_APP_DESC_MAGIC_WORD = 0xabcd5432,
    ESP_PARTITION_MAGIC = 0x50aa, SECTOR = 4096, IMAGE_SIZE = 10003,
};

#define OTA_WITH_SEQUENTIAL_WRITES 0xfffffffeU

typedef struct {
    int type, subtype;
    uint32_t address, size, erase_size;
    const char *label;
} esp_partition_t;

static const esp_partition_t slot_a = {
    ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, 0x10000, OTA_SLOT_SIZE, SECTOR, "ota_0",
};
static const esp_partition_t slot_b = {
    ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, 0x410000, OTA_SLOT_SIZE, SECTOR, "ota_1",
};
static const esp_partition_t data_partitions[] = {
    {ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, 0x810000, 0x2000, SECTOR, "otadata"},
    {ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x9000, 0x6000, SECTOR, "nvs"},
    {ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_PHY, 0xf000, 0x1000, SECTOR, "phy"},
};
static uint8_t flash[16 * 1024 * 1024];
static uint8_t baseline[sizeof(flash)];
static uint8_t expected[IMAGE_SIZE];
static const esp_partition_t *running, *inactive, *selected;
static unsigned touched_sectors, requests, selections, selection_attempts, ended, aborted;
static bool ppp_active, uart_open, at_ready, sms_ready, gps_ready;
static const char *scenario;

static bool is_case(const char *name)
{
    return strcmp(scenario, name) == 0;
}

static const esp_partition_t *esp_ota_get_running_partition(void) { return running; }
static const esp_partition_t *esp_ota_get_boot_partition(void) { return selected; }
static unsigned esp_ota_get_app_partition_count(void) { return 2; }

static const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *part)
{
    assert(part == running);
    return is_case("missing-slot") ? NULL : inactive;
}

static esp_err_t esp_ota_get_state_partition(const esp_partition_t *part, esp_ota_img_states_t *state)
{
    assert(part == running);
    *state = ESP_OTA_IMG_VALID;
    return ESP_OK;
}

static const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    assert(!label);
    for (size_t i = 0; i < sizeof(data_partitions) / sizeof(data_partitions[0]); ++i) {
        const esp_partition_t *part = &data_partitions[i];
        if (part->type == type && part->subtype == subtype) {
            return part;
        }
    }
    return NULL;
}

static esp_err_t esp_flash_get_physical_size(void *chip, uint32_t *size)
{
    assert(!chip);
    *size = sizeof(flash);
    return ESP_OK;
}

typedef struct __attribute__((packed)) {
    uint8_t magic, segment_count, spi_mode;
    uint8_t spi_speed : 4, spi_size : 4;
    uint8_t reserved[8];
    uint16_t chip_id;
    uint8_t tail[10];
} esp_image_header_t;
typedef struct { uint32_t addr, size; } esp_image_segment_header_t;
typedef struct {
    uint32_t magic_word, reserved[3];
    char version[32], project_name[32];
    uint8_t tail[176];
} esp_app_desc_t;
_Static_assert(sizeof(esp_image_header_t) == 24 && sizeof(esp_app_desc_t) == 256, "ESP image layout");

static const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t description = {.version = "1"};
    return &description;
}

static esp_err_t bootloader_common_check_chip_validity(const esp_image_header_t *image, int type)
{
    assert(type == ESP_IMAGE_APPLICATION);
    return image->chip_id == ESP_CHIP_ID_ESP32S3 ? ESP_OK : ESP_FAIL;
}

typedef struct ota_ops_entry {
    LIST_ENTRY(ota_ops_entry) entries;
    struct { const esp_partition_t *staging, *final; } partition;
    esp_ota_handle_t handle;
    size_t wrote_size, partial_bytes;
    uint8_t partial_data[16];
    bool need_erase, ota_resumption;
} ota_ops_entry_t;
static LIST_HEAD(ota_list, ota_ops_entry) s_ota_ops_entries_head;
static ota_ops_entry_t entry;
static const char flash_chip;
#define esp_flash_default_chip (&flash_chip)

static const esp_partition_t *esp_partition_verify(const esp_partition_t *part)
{
    return part == &slot_a || part == &slot_b ? part : NULL;
}

static bool is_ota_partition(const esp_partition_t *part) { return esp_partition_verify(part) != NULL; }
static bool esp_efuse_is_flash_encryption_enabled(void) { return false; }
static bool esp_ota_check_partition_conflict(const esp_partition_t *part)
{
    (void)part;
    return LIST_FIRST(&s_ota_ops_entries_head) != NULL;
}
static void esp_image_bootloader_offset_set(uint32_t address) { (void)address; abort(); }
static void esp_flash_set_dangerous_write_protection(const void *chip, bool enabled)
{
    (void)chip;
    (void)enabled;
    abort();
}
static ota_ops_entry_t *esp_ota_init_entry(const esp_partition_t *part)
{
    entry = (ota_ops_entry_t){.handle = 1, .partition = {.staging = part, .final = part}};
    LIST_INSERT_HEAD(&s_ota_ops_entries_head, &entry, entries);
    return &entry;
}

static esp_err_t esp_partition_erase_range(const esp_partition_t *part, size_t offset, size_t size)
{
    assert(ppp_active && part == inactive && offset + size <= part->size);
    assert(selected == running && offset % SECTOR == 0 && size % SECTOR == 0);
    if (is_case("flash-erase")) {
        assert(offset == 0);
        return ESP_ERR_FLASH_OP_FAIL;
    }
    touched_sectors += size / SECTOR;
    memset(flash + part->address + offset, 0xff, size);
    return ESP_OK;
}

static esp_err_t esp_partition_write(const esp_partition_t *part, size_t offset, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    assert(ppp_active && part == inactive && offset + size <= part->size);
    uint8_t *dest = flash + part->address + offset;
    bool interrupted = is_case("flash-partial-write") && offset != 0;
    if (interrupted) {
        assert(size >= 2);
        size /= 2;
    }
    for (size_t i = 0; i < size; ++i) {
        assert((dest[i] & bytes[i]) == bytes[i]);
        dest[i] = bytes[i];
    }
    return interrupted ? ESP_ERR_FLASH_OP_FAIL : ESP_OK;
}

static esp_err_t esp_partition_read(const esp_partition_t *part, size_t offset, void *data, size_t size)
{
    assert(part == inactive && offset + size <= part->size);
    memcpy(data, flash + part->address + offset, size);
    if (is_case("readback-digest") && !offset && size > 300) {
        ((uint8_t *)data)[300] ^= 1;
    }
    return ESP_OK;
}

#define TAG "sdk-ota"
#include "sdk_staging.inc"
#undef TAG

static esp_err_t esp_ota_end(esp_ota_handle_t handle)
{
    assert(handle == entry.handle && entry.wrote_size == IMAGE_SIZE);
    LIST_REMOVE(&entry, entries);
    ++ended;
    return is_case("invalid-image") ? ESP_ERR_OTA_VALIDATE_FAILED : ESP_OK;
}

static esp_err_t esp_ota_abort(esp_ota_handle_t handle)
{
    assert(handle == entry.handle && LIST_FIRST(&s_ota_ops_entries_head) == &entry);
    LIST_REMOVE(&entry, entries);
    ++aborted;
    return ESP_OK;
}

static esp_err_t esp_ota_set_boot_partition(const esp_partition_t *part)
{
    assert(part == inactive && !ppp_active && at_ready && sms_ready && gps_ready && ended == 1);
    assert(!memcmp(flash + part->address, expected, sizeof(expected)));
    assert(!LIST_FIRST(&s_ota_ops_entries_head));
    ++selection_attempts;
    if (is_case("selection-metadata")) {
        return ESP_ERR_FLASH_OP_FAIL;
    }
    ++selections;
    selected = part;
    return ESP_OK;
}

/* OpenSSL supplies real P-256 and SHA-256 behind the ESP-IDF crypto calls. */
typedef EVP_MD_CTX *psa_hash_operation_t;
typedef EVP_PKEY *mbedtls_pk_context;
typedef mbedtls_pk_context psa_key_attributes_t;
#define PSA_HASH_OPERATION_INIT NULL
#define PSA_KEY_ATTRIBUTES_INIT NULL
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 1
#define PSA_KEY_USAGE_VERIFY_HASH 2
#define PSA_ECC_FAMILY_SECP_R1 3
#define PSA_KEY_TYPE_ECC_PUBLIC_KEY(family) (family)
#define MBEDTLS_MD_SHA256 4

static int psa_crypto_init(void) { return PSA_SUCCESS; }
static int psa_hash_setup(psa_hash_operation_t *hash, int alg)
{
    assert(alg == PSA_ALG_SHA_256);
    *hash = EVP_MD_CTX_new();
    return *hash && EVP_DigestInit_ex(*hash, EVP_sha256(), NULL) == 1 ? PSA_SUCCESS : -1;
}
static int psa_hash_update(psa_hash_operation_t *hash, const void *data, size_t size)
{
    return EVP_DigestUpdate(*hash, data, size) == 1 ? PSA_SUCCESS : -1;
}
static int psa_hash_finish(psa_hash_operation_t *hash, uint8_t *out, size_t capacity, size_t *length)
{
    unsigned actual = 0;
    assert(capacity >= 32);
    int result = EVP_DigestFinal_ex(*hash, out, &actual);
    *length = actual;
    return result == 1 ? PSA_SUCCESS : -1;
}
static void psa_hash_abort(psa_hash_operation_t *hash) { EVP_MD_CTX_free(*hash); *hash = NULL; }
static int psa_hash_compute(int alg, const void *data, size_t size, uint8_t *out, size_t capacity, size_t *length)
{
    psa_hash_operation_t hash = NULL;
    int ok = !psa_hash_setup(&hash, alg) && !psa_hash_update(&hash, data, size) &&
             !psa_hash_finish(&hash, out, capacity, length);
    psa_hash_abort(&hash);
    return ok ? PSA_SUCCESS : -1;
}
static void mbedtls_pk_init(mbedtls_pk_context *key) { *key = NULL; }
static void mbedtls_pk_free(mbedtls_pk_context *key) { EVP_PKEY_free(*key); }
static int mbedtls_pk_parse_public_key(mbedtls_pk_context *key, const uint8_t *bytes, size_t size)
{
    BIO *bio = BIO_new_mem_buf(bytes, size);
    assert(bio);
    *key = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    BIO_free(bio);
    return *key ? 0 : -1;
}
static int mbedtls_pk_get_psa_attributes(mbedtls_pk_context *key, int usage, psa_key_attributes_t *attributes)
{
    assert(usage == PSA_KEY_USAGE_VERIFY_HASH);
    *attributes = *key;
    return 0;
}
static int psa_get_key_type(psa_key_attributes_t *attributes)
{
    char group[32];
    return EVP_PKEY_is_a(*attributes, "EC") &&
           EVP_PKEY_get_group_name(*attributes, group, sizeof(group), NULL) == 1 &&
           !strcmp(group, "prime256v1") ? PSA_ECC_FAMILY_SECP_R1 : 0;
}
static int psa_get_key_bits(psa_key_attributes_t *attributes) { return EVP_PKEY_get_bits(*attributes); }
static void psa_reset_key_attributes(psa_key_attributes_t *attributes) { *attributes = NULL; }
static int mbedtls_pk_verify(mbedtls_pk_context *key, int algorithm, const uint8_t *digest,
                             size_t length, const uint8_t *signature, size_t signature_size)
{
    assert(algorithm == MBEDTLS_MD_SHA256);
    EVP_PKEY_CTX *context = EVP_PKEY_CTX_new(*key, NULL);
    assert(context && EVP_PKEY_verify_init(context) == 1 &&
           EVP_PKEY_CTX_set_signature_md(context, EVP_sha256()) == 1);
    int ok = EVP_PKEY_verify(context, signature, signature_size, digest, length);
    EVP_PKEY_CTX_free(context);
    return ok == 1 ? 0 : -1;
}

typedef struct { int unused; } esp_netif_t;
typedef struct { int unused; } modem_t;
typedef struct { int port, tx_pin, rx_pin, baud_rate; } modem_config_t;
typedef struct {
    int port, tx_pin, rx_pin, baud_rate;
    const char *apn;
} cellular_net_config_t;
typedef struct { bool configured; } sms_control_t;
typedef esp_err_t (*cellular_operation_t)(esp_netif_t *, void *);

static int64_t esp_timer_get_time(void)
{
    struct timespec time;
    assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (int64_t)time.tv_sec * 1000000 + time.tv_nsec / 1000;
}
static bool cellular_ppp_is_uplink(esp_netif_t *netif)
{
    assert(netif);
    return ppp_active && !is_case("link-down");
}
static esp_err_t cellular_resolve_ipv4(const char *host, char *address, size_t size)
{
    assert(host && size >= sizeof("127.0.0.1"));
    strcpy(address, "127.0.0.1");
    return ESP_OK;
}
static esp_err_t esp_netif_get_netif_impl_name(esp_netif_t *netif, char *name)
{
    assert(netif);
    strcpy(name, "ppp0");
    return ESP_OK;
}

static const char *provisioning_apn(void)
{
    return is_case("missing-apn") || is_case("factory") ? NULL : FIXTURE_APN;
}

static bool cellular_apn_valid(const char *apn)
{
    return !strcmp(apn, FIXTURE_APN);
}

static esp_err_t cellular_net_session(const cellular_net_config_t *config, bool *recovered,
                                      cellular_operation_t operation, void *context)
{
    esp_netif_t netif = {0};
    assert(config && !strcmp(config->apn, FIXTURE_APN) && operation && !uart_open && !ppp_active);
    ppp_active = true;
    esp_err_t result = operation(&netif, context);
    ppp_active = false;
    *recovered = !is_case("pdp-cleanup-failure");
    return is_case("pdp-cleanup-deferred") ? ESP_FAIL : result;
}

enum { MODEM_FINAL_OK = 1 };
typedef struct { int final; } modem_result_t;
static modem_t fixture_modem;

static void modem_close(modem_t *modem)
{
    assert(modem == &fixture_modem && uart_open && !ppp_active);
    uart_open = false;
}

static esp_err_t modem_open(const modem_config_t *config, modem_t **modem)
{
    assert(config && !uart_open && !ppp_active);
    assert(!is_case("pdp-cleanup-failure") && !is_case("session-timeout"));
    if (is_case("uart-reopen-failure")) {
        return ESP_FAIL;
    }
    *modem = &fixture_modem;
    uart_open = true;
    return ESP_OK;
}

static esp_err_t modem_poll(modem_t *modem, unsigned timeout_ms)
{
    assert(modem == &fixture_modem && uart_open && timeout_ms == 250);
    return ESP_OK;
}

static esp_err_t modem_command(modem_t *modem, const char *command, const char *prefix,
                               unsigned timeout_ms, char *response, size_t size,
                               modem_result_t *result)
{
    assert(modem == &fixture_modem && uart_open && !ppp_active);
    assert(!strcmp(command, "AT") && !prefix && timeout_ms == 2000 && !response && !size);
    if (is_case("at-failure")) {
        return ESP_ERR_TIMEOUT;
    }
    at_ready = true;
    result->final = MODEM_FINAL_OK;
    return ESP_OK;
}

typedef bool StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
enum { pdPASS = 1, pdTRUE = 1 };
#define pdMS_TO_TICKS(ms) (ms)

static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    *storage = false;
    return storage;
}
static void xSemaphoreGive(SemaphoreHandle_t done) { *done = true; }
static int xSemaphoreTake(SemaphoreHandle_t done, unsigned timeout_ms)
{
    assert(*done && timeout_ms == 480000);
    return !is_case("session-timeout");
}

static int xTaskCreate(void (*worker)(void *), const char *name, unsigned stack_size,
                       void *context, unsigned priority, void *handle)
{
    assert(strcmp(name, "ota_session") == 0 && stack_size == 24 * 1024);
    assert(priority == 4 && !handle);
    if (is_case("task-allocation-failure")) {
        return 0;
    }
    worker(context);
    return pdPASS;
}
static unsigned uxTaskGetStackHighWaterMark(void *task)
{
    assert(!task);
    return 4096;
}
static void vTaskDelete(void *task) { assert(!task); }

// The worker executes synchronously here. The adapter injects recovery and
// timeout outcomes while preserving the real wrapper's UART and result gates.
#define TAG LTE_TAG
#include "lte_wrapper.inc"
#undef TAG

static void sms_control_resume(sms_control_t *sms, modem_t *modem)
{
    assert(at_ready && !ppp_active && modem);
    sms->configured = sms_ready = !is_case("sms-failure");
}
static bool start_gps(modem_t *modem)
{
    assert(at_ready && !ppp_active && modem);
    return gps_ready = !is_case("gnss-failure");
}

enum { HTTP_EVENT_ON_HEADER };
typedef struct {
    int event_id;
    const char *header_key, *header_value;
    void *user_data;
} esp_http_client_event_t;
typedef struct {
    const char *url, *common_name;
    int (*crt_bundle_attach)(void *);
    const struct ifreq *if_name;
    int timeout_ms;
    bool disable_auto_redirect;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
} esp_http_client_config_t;

typedef struct {
    esp_http_client_config_t config;
    char host[254];
    uint8_t bytes[16384];
    size_t size, offset;
    long status;
    curl_off_t length;
    CURLcode result;
} host_http_t;
typedef host_http_t *esp_http_client_handle_t;

static int esp_crt_bundle_attach(void *config) { (void)config; return 0; }

static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    assert(config->disable_auto_redirect && config->crt_bundle_attach == esp_crt_bundle_attach);
    assert(!strcmp(config->if_name->ifr_name, "ppp0"));
    host_http_t *client = calloc(1, sizeof(*client));
    assert(client);
    client->config = *config;
    return client;
}

static esp_err_t esp_http_client_set_header(host_http_t *client, const char *key, const char *value)
{
    if (!strcmp(key, "Host")) {
        assert(strlen(value) < sizeof(client->host));
        strcpy(client->host, value);
    } else {
        assert(!strcmp(key, "Connection") && !strcmp(value, "close"));
    }
    return ESP_OK;
}

static size_t host_header(char *bytes, size_t size, size_t count, void *context)
{
    host_http_t *client = context;
    size_t length = size * count;
    if (length > 12 && !strncasecmp(bytes, "Location: ", 10)) {
        char *value = strndup(bytes + 10, length - 12);
        assert(value);
        esp_http_client_event_t event = {HTTP_EVENT_ON_HEADER, "Location", value, client->config.user_data};
        client->config.event_handler(&event);
        free(value);
    }
    return length;
}

static size_t host_body(char *bytes, size_t size, size_t count, void *context)
{
    host_http_t *client = context;
    size_t length = size * count;
    assert(length <= sizeof(client->bytes) - client->size);
    memcpy(client->bytes + client->size, bytes, length);
    client->size += length;
    return length;
}

static esp_err_t esp_http_client_open(host_http_t *client, int payload)
{
    assert(!payload && !strcmp(client->host, client->config.common_name));
    static const char prefix[] = "https://127.0.0.1/";
    assert(!strncmp(client->config.url, prefix, sizeof(prefix) - 1));
    const char *path = client->config.url + sizeof(prefix) - 2;
    const char *port = getenv("OTA_TEST_PORT");
    char url[OTA_URL_MAX + 32], resolve[300];
    snprintf(url, sizeof(url), "https://%s:%s%s", client->config.common_name, port, path);
    snprintf(resolve, sizeof(resolve), "%s:%s:127.0.0.1", client->config.common_name, port);
    CURL *curl = curl_easy_init();
    assert(curl);
    struct curl_slist *mapping = curl_slist_append(NULL, resolve);
    char host_header_value[260];
    snprintf(host_header_value, sizeof(host_header_value), "Host: %s", client->host);
    struct curl_slist *headers = curl_slist_append(NULL, host_header_value);
    assert(mapping && headers);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_RESOLVE, mapping);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_CAINFO, getenv("OTA_TEST_CA"));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)client->config.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, host_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, client);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, host_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, client);
    ++requests;
    client->result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &client->status);
    if (getenv("OTA_TEST_VERBOSE")) {
        fprintf(stderr, "HTTP %s %ld: %s\n", client->host, client->status, curl_easy_strerror(client->result));
    }
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &client->length);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    curl_slist_free_all(mapping);
    if (is_case("wrong-cert") && !strcmp(client->host, "wrong.example")) {
        assert(client->result == CURLE_PEER_FAILED_VERIFICATION);
    }
    if (is_case("untrusted-cert")) {
        assert(client->result == CURLE_PEER_FAILED_VERIFICATION);
    }
    // ESP-IDF opens the connection before reading the body. A transfer that
    // received headers and some bytes must fail during the simulated read.
    bool interrupted_body = client->status == 200 && client->size > 0 &&
                            (client->result == CURLE_PARTIAL_FILE || client->result == CURLE_RECV_ERROR);
    return client->result == CURLE_OK || interrupted_body ? ESP_OK : ESP_FAIL;
}

static int64_t esp_http_client_fetch_headers(host_http_t *client)
{
    return client->length < 0 ? 0 : client->length;
}
static int esp_http_client_get_status_code(host_http_t *client) { return client->status; }
static int esp_http_client_read(host_http_t *client, char *buffer, size_t size)
{
    size_t remaining = client->size - client->offset;
    size_t count = MIN(remaining, MIN(size, 701));
    memcpy(buffer, client->bytes + client->offset, count);
    client->offset += count;
    return count;
}
static bool esp_http_client_is_complete_data_received(host_http_t *client)
{
    return client->result == CURLE_OK && client->offset == client->size;
}
static esp_err_t esp_http_client_cleanup(host_http_t *client)
{
    free(client);
    return ESP_OK;
}

static bool cellular_modem_maintenance(modem_t **modem, const char *line, const char *apn)
{
    (void)modem;
    (void)line;
    (void)apn;
    return false;
}

#include "installer.inc"

int main(int argc, char **argv)
{
    assert(argc == 3);
    scenario = argv[1];
    FILE *image = fopen(argv[2], "rb");
    assert(image && fread(expected, 1, sizeof(expected), image) == sizeof(expected));
    assert(fgetc(image) == EOF && fclose(image) == 0);
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    running = is_case("success-from-b") ? &slot_b : &slot_a;
    selected = running;
    inactive = running == &slot_a ? &slot_b : &slot_a;
    for (size_t i = 0; i < sizeof(flash); ++i) {
        flash[i] = (uint8_t)(i ^ (i >> 12));
    }
    memset(flash + inactive->address, 0, inactive->size);
    memcpy(baseline, flash, sizeof(flash));
    uart_open = true;
    modem_config_t config = {0};
    sms_control_t sms = {.configured = !is_case("factory")};
    bool repeated_check = is_case("dry-run-repeated");
    const char *command = is_case("dry-run") || repeated_check ? "ota check 2" : "ota install powered 2";
    modem_t *result = run_update(&fixture_modem, &config, &sms, command);
    if (repeated_check) {
        assert(result == &fixture_modem && selected == running && touched_sectors == 0);
        assert(at_ready && sms_ready && gps_ready && requests == 4);
        result = run_update(result, &config, &sms, command);
    }

    bool success = is_case("success") || is_case("success-from-b") || is_case("chunked-image");
    bool modem_unavailable = is_case("at-failure") || is_case("pdp-cleanup-failure") ||
                             is_case("uart-reopen-failure") || is_case("session-timeout");
    bool selection_failure = is_case("selection-metadata");
    bool staging_failure = is_case("flash-erase") || is_case("flash-partial-write");
    bool staged = success || selection_failure || modem_unavailable || is_case("sms-failure") ||
                  is_case("gnss-failure") || is_case("readback-digest") || is_case("invalid-image") ||
                  is_case("pdp-cleanup-deferred");
    assert((result == NULL) == modem_unavailable);
    assert(uart_open == !modem_unavailable);
    assert(selected == (success ? inactive : running) && selections == (unsigned)success);
    assert(selection_attempts == (unsigned)(success || selection_failure));
    if (staging_failure) {
        assert(at_ready && sms_ready && gps_ready);
        assert(!strcmp(update_report, "Update stopped (ESP_ERR_FLASH_OP_FAIL, SMS ready=1, GNSS ready=1); boot selection unchanged"));
    } else if (selection_failure) {
        assert(!strcmp(update_report, "Could not select the candidate: ESP_ERR_FLASH_OP_FAIL"));
    }
    assert(!ppp_active && !LIST_FIRST(&s_ota_ops_entries_head));
    // The fixture models metadata selection separately. All other flash, including NVS,
    // the full running image and the bootloader, must retain its original contents.
    assert(!memcmp(flash, baseline, inactive->address));
    size_t end = inactive->address + inactive->size;
    assert(!memcmp(flash + end, baseline + end, sizeof(flash) - end));
    if (staged) {
        assert(ended == 1 && aborted == 0 && touched_sectors == 3);
        assert(!memcmp(flash + inactive->address, expected, sizeof(expected)));
        for (size_t i = sizeof(expected); i < 3 * SECTOR; ++i) {
            assert(flash[inactive->address + i] == 0xff);
        }
        assert(!memcmp(flash + inactive->address + 3 * SECTOR,
                       baseline + inactive->address + 3 * SECTOR, inactive->size - 3 * SECTOR));
    } else if (staging_failure) {
        assert(aborted == 1 && ended == 0);
        const uint8_t *target = flash + inactive->address;
        if (is_case("flash-erase")) {
            assert(touched_sectors == 0 && !memcmp(target, baseline + inactive->address, inactive->size));
        } else {
            assert(touched_sectors > 0 && touched_sectors < 3);
            size_t prefix = 0;
            while (prefix < sizeof(expected) && target[prefix] == expected[prefix]) {
                ++prefix;
            }
            assert(prefix > 288 && prefix < sizeof(expected));
            size_t erased_end = touched_sectors * SECTOR;
            for (size_t i = prefix; i < erased_end; ++i) {
                assert(target[i] == 0xff);
            }
            assert(!memcmp(target + erased_end, baseline + inactive->address + erased_end,
                           inactive->size - erased_end));
        }
    } else if (is_case("altered") || is_case("truncated")) {
        assert(aborted == 1 && ended == 0 && touched_sectors == (is_case("altered") ? 3U : 2U));
        size_t untouched = touched_sectors * SECTOR;
        assert(!memcmp(flash + inactive->address + untouched,
                       baseline + inactive->address + untouched, inactive->size - untouched));
    } else {
        assert(touched_sectors == 0 && ended == 0 && aborted == 0);
        assert(!memcmp(flash + inactive->address, baseline + inactive->address, inactive->size));
    }
    if (is_case("dry-run") || repeated_check) {
        assert(at_ready && sms_ready && gps_ready && requests == (repeated_check ? 8 : 4));
    } else if (is_case("pdp-cleanup-deferred")) {
        assert(at_ready && sms_ready && gps_ready && requests == 6);
    } else if (is_case("link-down") || is_case("missing-slot") ||
               is_case("factory") || is_case("missing-apn") || is_case("task-allocation-failure")) {
        assert(requests == 0);
    } else if (is_case("redirect-loop")) {
        assert(requests == 4);
    } else if (is_case("redirect-http") || is_case("redirect-host") ||
               is_case("duplicate-location") || is_case("untrusted-cert")) {
        assert(requests == 1);
    } else if (is_case("wrong-cert")) {
        assert(requests == 2);
    } else if (is_case("bad-signature")) {
        assert(requests == 4);
    } else {
        assert(requests == 6);
    }
    printf("Installer integration: %s passed\n", scenario);
    curl_global_cleanup();
}
