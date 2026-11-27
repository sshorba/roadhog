#include "ota_update.h"

#ifdef CONFIG_ROADHOG_OTA_UPDATE_BENCH
#include "ota_manifest.h"
#include "ota_sms.h"
#include "ota_report.h"
#include "connection_status.h"
#include "lte_diagnostic.h"
#include "provisioning.h"

#include <inttypes.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bootloader_common.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_flash.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_random.h"
#include "bootloader_random.h"
#include "mbedtls/platform_util.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/pk.h"
#include "psa/crypto.h"

#define TRANSFER_US INT64_C(300000000)
#define STAGING_DEADLINE_US INT64_C(420000000)
#define HTTP_BUFFER 1024
#define MAX_REDIRECTS 3
#define SIGNATURE_MAX 72
#define IMAGE_PREFIX (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

static const char *TAG = "ota_update";
extern const uint8_t public_key_start[] asm("_binary_ota_public_pem_start");
extern const uint8_t public_key_end[] asm("_binary_ota_public_pem_end");

typedef struct {
    uint32_t sequence;
    bool install;
    int64_t staging_deadline;
    const esp_partition_t *running;
    const esp_partition_t *target;
#if defined(CONFIG_ROADHOG_OTA_SMS_BENCH)
    bool remote;
    ota_manifest_t release;
#endif
} update_t;

typedef esp_err_t (*consume_t)(void *, const uint8_t *, size_t);

typedef struct {
    size_t maximum;
    size_t exact;
    size_t used;
    consume_t consume;
    void *context;
} body_t;

typedef struct {
    char location[OTA_URL_MAX];
    bool invalid;
    uint8_t body_buffer[HTTP_BUFFER];
} redirect_t;

static esp_err_t on_header(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_HEADER &&
        !strcasecmp(event->header_key, "Location")) {
        redirect_t *redirect = event->user_data;
        size_t length = strlen(event->header_value);
        if (!length || length >= sizeof(redirect->location) || redirect->location[0]) {
            redirect->invalid = true;
        } else {
            memcpy(redirect->location, event->header_value, length + 1);
        }
    }
    return ESP_OK;
}

static bool transfer_ready(esp_netif_t *netif, int64_t deadline)
{
    return esp_timer_get_time() < deadline && cellular_ppp_is_uplink(netif);
}

static esp_err_t read_body(esp_http_client_handle_t client, int64_t content_length,
                           esp_netif_t *netif, int64_t deadline, body_t *body, uint8_t *buffer)
{
    if (content_length > (int64_t)body->maximum ||
        (content_length > 0 && body->exact && content_length != (int64_t)body->exact)) {
        return ESP_ERR_INVALID_SIZE;
    }

    while (transfer_ready(netif, deadline)) {
        int count = esp_http_client_read(client, (char *)buffer, HTTP_BUFFER);
        if (!transfer_ready(netif, deadline)) {
            return ESP_ERR_TIMEOUT;
        }
        if (count < 0) {
            return ESP_FAIL;
        }
        if (count > 0) {
            if ((size_t)count > body->maximum - body->used) {
                return ESP_ERR_INVALID_SIZE;
            }
            esp_err_t error = body->consume(body->context, buffer, count);
            if (error != ESP_OK) {
                return error;
            }
            body->used += count;
        }
        if (esp_http_client_is_complete_data_received(client)) {
            return body->used && (!body->exact || body->used == body->exact)
                   ? ESP_OK : ESP_ERR_INVALID_SIZE;
        }
        if (!count) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static bool redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

static esp_err_t download(esp_netif_t *netif, char *url, int64_t deadline, body_t *body)
{
    // Heap storage keeps URL and header buffers off the PPP worker stack.
    redirect_t *redirect = calloc(1, sizeof(*redirect));
    char *numeric_url = malloc(OTA_URL_MAX + 16);
    if (!redirect || !numeric_url) {
        free(redirect);
        free(numeric_url);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = ESP_ERR_INVALID_RESPONSE;
    for (unsigned hop = 0; hop <= MAX_REDIRECTS; ++hop) {
        char host[254], address[16];
        const char *path;
        if (!ota_https_url(url, host, &path) ||
            !ota_host_allowed(host, CONFIG_ROADHOG_OTA_RELEASE_HOSTS)) {
            error = ESP_ERR_INVALID_ARG;
            break;
        }
        if (!transfer_ready(netif, deadline)) {
            error = ESP_ERR_TIMEOUT;
            break;
        }
        error = cellular_resolve_ipv4(host, address, sizeof(address));
        if (error != ESP_OK) {
            break;
        }
        struct ifreq interface = {0};
        if (!transfer_ready(netif, deadline) ||
            esp_netif_get_netif_impl_name(netif, interface.ifr_name) != ESP_OK) {
            error = ESP_ERR_INVALID_STATE;
            break;
        }
        snprintf(numeric_url, OTA_URL_MAX + 16, "https://%s%s", address, path);
        redirect->location[0] = '\0';
        redirect->invalid = false;
        esp_http_client_config_t config = {
            .url = numeric_url,
            .common_name = host,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .if_name = &interface,
            .timeout_ms = 3000,
            .disable_auto_redirect = true,
            .event_handler = on_header,
            .user_data = redirect,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            error = ESP_ERR_NO_MEM;
            break;
        }
        error = esp_http_client_set_header(client, "Host", host);
        if (error == ESP_OK) {
            error = esp_http_client_set_header(client, "Connection", "close");
        }
        if (error == ESP_OK) {
            error = esp_http_client_open(client, 0);
        }
        bool follow = false;
        if (error == ESP_OK) {
            int64_t length = esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);
            if (length < 0 || !transfer_ready(netif, deadline)) {
                error = ESP_ERR_INVALID_RESPONSE;
            } else if (status == 200) {
                error = read_body(client, length, netif, deadline, body, redirect->body_buffer);
            } else {
                follow = redirect_status(status) && hop < MAX_REDIRECTS &&
                         !redirect->invalid && redirect->location[0];
                error = status == 404 ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_RESPONSE;
                ESP_LOGI(TAG, "HTTP status %d at hop %u", status, hop);
            }
        }
        esp_err_t close_error = esp_http_client_cleanup(client);
        if (close_error != ESP_OK) {
            error = close_error;
            break;
        }
        if (!follow) {
            break;
        }
        strcpy(url, redirect->location);
    }
    free(redirect);
    free(numeric_url);
    return error;
}

static esp_err_t copy_bytes(void *destination, const uint8_t *bytes, size_t size)
{
    uint8_t **next = destination;
    memcpy(*next, bytes, size);
    *next += size;
    return ESP_OK;
}

static bool asset_url(char *url, const update_t *update, const char *suffix)
{
    const char *base = CONFIG_ROADHOG_OTA_RELEASE_BASE;
#if defined(CONFIG_ROADHOG_OTA_SMS_BENCH)
    if (update->remote) {
        base = CONFIG_ROADHOG_OTA_SMS_RELEASE_BASE;
    }
    if (update->remote && !update->install) {
        int length = snprintf(url, OTA_URL_MAX, "%s/latest", base);
        return length > 0 && length < OTA_URL_MAX;
    }
#endif
    int length = snprintf(url, OTA_URL_MAX, "%s/v%" PRIu32 "/roadhog-v%" PRIu32 ".%s",
                          base, update->sequence, update->sequence, suffix);
    return length > 0 && length < OTA_URL_MAX;
}

static esp_err_t download_bytes(esp_netif_t *netif, const update_t *update, int64_t deadline,
                                uint8_t *bytes, body_t *body, const char *suffix, char *url)
{
    body->consume = copy_bytes;
    body->context = &bytes;
    return asset_url(url, update, suffix)
           ? download(netif, url, deadline, body) : ESP_ERR_INVALID_ARG;
}

static bool verify_signature(const uint8_t *manifest, size_t length,
                             const uint8_t *signature, size_t signature_size)
{
    uint8_t digest[32];
    size_t digest_size = 0;
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    bool valid = psa_crypto_init() == PSA_SUCCESS &&
        psa_hash_compute(PSA_ALG_SHA_256, manifest, length, digest, sizeof(digest), &digest_size) == PSA_SUCCESS &&
        mbedtls_pk_parse_public_key(&key, public_key_start, public_key_end - public_key_start) == 0 &&
        mbedtls_pk_get_psa_attributes(&key, PSA_KEY_USAGE_VERIFY_HASH, &attributes) == 0 &&
        psa_get_key_type(&attributes) == PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1) &&
        psa_get_key_bits(&attributes) == 256 &&
        mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, digest_size, signature, signature_size) == 0;
    psa_reset_key_attributes(&attributes);
    mbedtls_pk_free(&key);
    return valid;
}

typedef struct {
    const update_t *update;
    esp_ota_handle_t handle;
    bool open;
    uint8_t prefix[IMAGE_PREFIX + sizeof(OTA_SMS_IMAGE_MARKER)];
    size_t prefix_used;
    size_t written;
    psa_hash_operation_t hash;
} writer_t;

static bool candidate_header(const writer_t *writer)
{
    esp_image_header_t image;
    esp_app_desc_t description;
    memcpy(&image, writer->prefix, sizeof(image));
    memcpy(&description, writer->prefix + IMAGE_PREFIX - sizeof(description), sizeof(description));
    uint32_t sequence;
    return image.magic == ESP_IMAGE_HEADER_MAGIC &&
           image.chip_id == ESP_CHIP_ID_ESP32S3 &&
           image.spi_size == ESP_IMAGE_FLASH_SIZE_16MB &&
           bootloader_common_check_chip_validity(&image, ESP_IMAGE_APPLICATION) == ESP_OK &&
           description.magic_word == ESP_APP_DESC_MAGIC_WORD &&
           memchr(description.version, 0, sizeof(description.version)) &&
           ota_sequence_parse(description.version, &sequence) && sequence == writer->update->sequence &&
           strncmp(description.project_name, "roadhog", sizeof(description.project_name)) == 0;
}

static esp_err_t write_chunk(writer_t *writer, const uint8_t *bytes, size_t length)
{
    if (psa_hash_update(&writer->hash, bytes, length) != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    esp_err_t error = esp_ota_write(writer->handle, bytes, length);
    if (error == ESP_OK) {
        size_t previous = writer->written;
        writer->written += length;
        if (previous / 65536 != writer->written / 65536) {
            ESP_LOGI(TAG, "Staged %zu bytes", writer->written);
        }
    }
    return error;
}

static esp_err_t write_body(void *context, const uint8_t *bytes, size_t length)
{
    writer_t *writer = context;
    if (!writer->open) {
        size_t prefix_size = IMAGE_PREFIX;
#if defined(CONFIG_ROADHOG_OTA_SMS_BENCH)
        if (writer->update->remote) {
            prefix_size += sizeof(OTA_SMS_IMAGE_MARKER);
        }
#endif
        size_t missing = prefix_size - writer->prefix_used;
        size_t keep = length < missing ? length : missing;
        memcpy(writer->prefix + writer->prefix_used, bytes, keep);
        writer->prefix_used += keep;
        if (writer->prefix_used != prefix_size) {
            return ESP_OK;
        }
        if (!candidate_header(writer) ||
            (prefix_size > IMAGE_PREFIX &&
             memcmp(writer->prefix + IMAGE_PREFIX, OTA_SMS_IMAGE_MARKER, sizeof(OTA_SMS_IMAGE_MARKER)))) {
            return ESP_ERR_INVALID_VERSION;
        }
        // In IDF v6.1.1 begin can erase the inactive otadata sector.
        // Offset zero with sequential writes stages from scratch without that side effect.
        esp_err_t error = esp_ota_resume(writer->update->target, OTA_WITH_SEQUENTIAL_WRITES,
                                         0, &writer->handle);
        if (error != ESP_OK) {
            return error;
        }
        writer->open = true;
        error = write_chunk(writer, writer->prefix, prefix_size);
        if (error != ESP_OK || length == keep) {
            return error;
        }
        bytes += keep;
        length -= keep;
    }
    return write_chunk(writer, bytes, length);
}

static esp_err_t stage_image(esp_netif_t *netif, const update_t *update, int64_t deadline,
                             const ota_manifest_t *manifest, char *url)
{
    writer_t writer = {.update = update, .hash = PSA_HASH_OPERATION_INIT};
    if (manifest->size < IMAGE_PREFIX || !asset_url(url, update, "bin") ||
        psa_hash_setup(&writer.hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return ESP_ERR_INVALID_SIZE;
    }
    body_t body = {
        .maximum = manifest->size, .exact = manifest->size,
        .consume = write_body, .context = &writer,
    };
    esp_err_t error = download(netif, url, deadline, &body);
    uint8_t digest[32];
    size_t length;
    if (error == ESP_OK && (!writer.open || writer.written != manifest->size ||
        psa_hash_finish(&writer.hash, digest, sizeof(digest), &length) != PSA_SUCCESS ||
        length != sizeof(digest) || memcmp(digest, manifest->digest, sizeof(digest)))) {
        error = ESP_ERR_INVALID_CRC;
    }
    psa_hash_abort(&writer.hash);
    if (writer.open) {
        if (error == ESP_OK) {
            error = esp_ota_end(writer.handle);
        } else {
            esp_ota_abort(writer.handle);
        }
    }
    if (error == ESP_OK) {
        error = ota_verify_slot(update->target, manifest, deadline);
        if (error == ESP_OK) {
            ESP_LOGI(TAG, "Validated image and SHA-256 readback in %s", update->target->label);
        }
    }
    return error;
}

static esp_err_t update_over_ppp(esp_netif_t *netif, void *context)
{
    update_t *update = context;
    int64_t deadline = esp_timer_get_time() + TRANSFER_US;
    if (deadline > update->staging_deadline) {
        deadline = update->staging_deadline;
    }
    struct {
        uint8_t manifest_bytes[OTA_MANIFEST_MAX + SIGNATURE_MAX + 1];
        uint8_t signature[SIGNATURE_MAX];
        ota_manifest_t manifest;
        char url[OTA_URL_MAX];
    } *workspace = malloc(sizeof(*workspace));
    if (!workspace) {
        return ESP_ERR_NO_MEM;
    }
    body_t manifest_body = {.maximum = OTA_MANIFEST_MAX};
    body_t signature_body = {.maximum = sizeof(workspace->signature)};
    const uint8_t *manifest_bytes = workspace->manifest_bytes;
    const uint8_t *signature = workspace->signature;
    bool discovery = false;
    size_t minimum_image_size = IMAGE_PREFIX;
    bool (*parse)(const uint8_t *, size_t, ota_manifest_t *) = ota_manifest_parse;
#if defined(CONFIG_ROADHOG_OTA_SMS_BENCH)
    if (update->remote) {
        parse = ota_manifest_parse_sms;
        minimum_image_size += sizeof(OTA_SMS_IMAGE_MARKER);
    }
    discovery = update->remote && !update->install;
    if (discovery) {
        manifest_body.maximum = sizeof(workspace->manifest_bytes);
    }
#endif
    ota_manifest_t *manifest = &workspace->manifest;
    esp_err_t error = download_bytes(netif, update, deadline, workspace->manifest_bytes,
                                     &manifest_body, "manifest", workspace->url);
    if (error == ESP_OK && discovery) {
        // One object makes the signed pointer atomic at publication and retrieval.
        signature_body.used = manifest_body.used ? manifest_bytes[0] : 0;
        if (!signature_body.used || signature_body.used > SIGNATURE_MAX ||
            manifest_body.used <= 1 + signature_body.used) {
            error = ESP_ERR_INVALID_RESPONSE;
        } else {
            signature = manifest_bytes + 1;
            manifest_bytes += 1 + signature_body.used;
            manifest_body.used -= 1 + signature_body.used;
        }
    } else if (error == ESP_OK) {
        error = download_bytes(netif, update, deadline, workspace->signature,
                               &signature_body, "sig", workspace->url);
    }
    if (error == ESP_OK &&
        (manifest_body.used > OTA_MANIFEST_MAX ||
         !verify_signature(manifest_bytes, manifest_body.used, signature, signature_body.used) ||
         !parse(manifest_bytes, manifest_body.used, manifest) ||
         (!discovery && manifest->sequence != update->sequence) || manifest->size < minimum_image_size)) {
        error = ESP_ERR_INVALID_RESPONSE;
    }
#if defined(CONFIG_ROADHOG_OTA_SMS_BENCH)
    if (error == ESP_OK && discovery) {
        update->release = *manifest;
    } else if (error == ESP_OK && update->remote && !ota_sms_same_release(&update->release, manifest)) {
        error = ESP_ERR_INVALID_RESPONSE;
    }
#endif
    if (error == ESP_OK) {
        char digest[65];
        for (size_t i = 0; i < sizeof(manifest->digest); ++i) {
            snprintf(digest + 2 * i, 3, "%02x", manifest->digest[i]);
        }
        ESP_LOGI(TAG, "Verified manifest v%" PRIu32 ", %s, %" PRIu32 " bytes, SHA-256 %s",
                 manifest->sequence, update->target->label, manifest->size, digest);
        if (update->install) {
            error = stage_image(netif, update, deadline, manifest, workspace->url);
        }
    }
    free(workspace);
    return error == ESP_OK && !transfer_ready(netif, deadline) ? ESP_ERR_TIMEOUT : error;
}

static bool slot_is(const esp_partition_t *slot, esp_partition_subtype_t subtype, uint32_t address)
{
    return slot && slot->subtype == subtype && slot->address == address &&
           slot->size == OTA_SLOT_SIZE;
}

static bool data_partition_is(esp_partition_subtype_t subtype, uint32_t address, uint32_t size)
{
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, subtype, NULL);
    return partition && partition->address == address && partition->size == size;
}

static bool update_ready(update_t *update)
{
    uint32_t current, flash_size;
    esp_ota_img_states_t state;
    update->running = esp_ota_get_running_partition();
    update->target = esp_ota_get_next_update_partition(update->running);
    bool running_a = slot_is(update->running, ESP_PARTITION_SUBTYPE_APP_OTA_0, 0x10000);
    bool layout_ok = running_a ? slot_is(update->target, ESP_PARTITION_SUBTYPE_APP_OTA_1, 0x410000)
                               : slot_is(update->running, ESP_PARTITION_SUBTYPE_APP_OTA_1, 0x410000) &&
                                 slot_is(update->target, ESP_PARTITION_SUBTYPE_APP_OTA_0, 0x10000);
    return layout_ok && esp_ota_get_app_partition_count() == 2 &&
           data_partition_is(ESP_PARTITION_SUBTYPE_DATA_OTA, 0x810000, 0x2000) &&
           data_partition_is(ESP_PARTITION_SUBTYPE_DATA_NVS, 0x9000, 0x6000) &&
           data_partition_is(ESP_PARTITION_SUBTYPE_DATA_PHY, 0xf000, 0x1000) &&
           esp_flash_get_physical_size(NULL, &flash_size) == ESP_OK && flash_size == 16 * 1024 * 1024 &&
           esp_ota_get_boot_partition() == update->running &&
           esp_ota_get_state_partition(update->running, &state) == ESP_OK && state == ESP_OTA_IMG_VALID &&
           ota_sequence_parse(esp_app_get_description()->version, &current) &&
           (update->sequence == 0 || update->sequence > current);
}

#if defined(CONFIG_ROADHOG_OTA_SMS_BENCH)
static ota_sms_challenge_t challenge;
static ota_sms_command_t queued;
static ota_manifest_t confirmed;
static const char *remote_status;
static int64_t queued_until;
static update_t staged;
static int64_t staged_until;
static mbedtls_svc_key_id_t challenge_key;

void ota_sms_init(void)
{
    // Collect true entropy before the portal can enable the Wi-Fi radio.
    uint8_t seed[32];
    bootloader_random_enable();
    esp_fill_random(seed, sizeof(seed));
    bootloader_random_disable();

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_import_key(&attributes, seed, sizeof(seed), &challenge_key) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "SMS OTA challenge generator unavailable");
    }
    psa_reset_key_attributes(&attributes);
    mbedtls_platform_zeroize(seed, sizeof(seed));
}

static bool create_challenge(const ota_manifest_t *release)
{
    static uint64_t counter;
    ++counter;
    uint8_t digest[32];
    size_t length;
    bool ok = psa_mac_compute(challenge_key, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                              (const uint8_t *)&counter, sizeof(counter),
                              digest, sizeof(digest), &length) == PSA_SUCCESS &&
              length == sizeof(digest);
    if (ok) {
        ota_sms_issue(&challenge, release, digest, esp_timer_get_time());
    }
    mbedtls_platform_zeroize(digest, sizeof(digest));
    return ok;
}

void ota_sms_ack_failed(void)
{
    if (queued != OTA_SMS_NONE || staged.install) {
        staged.install = false;
        queued = OTA_SMS_NONE;
        confirmed = (ota_manifest_t){0};
        remote_status = "Queued OTA request stopped after an SMS acknowledgement failure.";
    }
}

static void expire_remote_work(int64_t now)
{
    if ((queued != OTA_SMS_NONE && now >= queued_until) ||
        (staged.install && now >= staged_until)) {
        staged.install = false;
        queued = OTA_SMS_NONE;
        confirmed = (ota_manifest_t){0};
        remote_status = "Queued OTA request expired.";
    }
}

bool ota_sms_service_command(const sms_control_t *sms, modem_t *modem, const sms_request_t *request)
{
    ota_sms_command_t command = ota_sms_parse(request->text);
    if (command == OTA_SMS_NONE) {
        return false;
    }
    if (!sms->owner || strcmp(sms->owner, request->sender) != 0) {
        return true;
    }

    const char *reply;
    char detail[SMS_TEXT_CAPACITY];
    ota_sms_command_t enqueue = OTA_SMS_NONE;
    int64_t now = esp_timer_get_time();
    int64_t expires = now + OTA_SMS_LIFETIME_US;
    bool had_code = challenge.expires != 0;
    bool pending = ota_sms_pending(&challenge, now);
    if (had_code && !pending) {
        remote_status = "OTA confirmation expired.";
    }
    expire_remote_work(now);
    if (command == OTA_SMS_CANCEL) {
        staged.install = false;
        challenge = (ota_sms_challenge_t){0};
        queued = OTA_SMS_NONE;
        confirmed = (ota_manifest_t){0};
        remote_status = "OTA request cancelled.";
        reply = remote_status;
    } else if (command == OTA_SMS_STATUS) {
        uint32_t current;
        if (!ota_sequence_parse(esp_app_get_description()->version, &current)) {
            reply = "Current firmware sequence unavailable.";
        } else {
            const char *status = remote_status ? remote_status : ota_sms_boot_status();
            if (pending) {
                status = "Awaiting confirmation. Send OTA CHECK for a new code.";
            }
            snprintf(detail, sizeof(detail), "Current v%" PRIu32 ". %s", current, status);
            reply = detail;
        }
    } else if (queued != OTA_SMS_NONE || staged.install) {
        reply = "An OTA request is queued.";
    } else if (command == OTA_SMS_CHECK) {
        challenge = (ota_sms_challenge_t){0};
        update_t check = {0};
        char host[254];
        const char *path;
        if (!update_ready(&check) ||
            !ota_https_url(CONFIG_ROADHOG_OTA_SMS_RELEASE_BASE, host, &path) ||
            strchr(path, '?') || path[strlen(path) - 1] == '/' ||
            !ota_host_allowed(host, CONFIG_ROADHOG_OTA_RELEASE_HOSTS)) {
            remote_status = "OTA check unavailable. Check configuration and running firmware.";
            reply = remote_status;
        } else {
            enqueue = OTA_SMS_CHECK;
            reply = "Checking for signed firmware. SMS may be temporarily unavailable.";
        }
    } else if (command == OTA_SMS_CONFIRM) {
        expires = challenge.expires;
        if (!ota_sms_confirm(&challenge, request->text + 12, now, &confirmed)) {
            reply = "Invalid or expired OTA code. Send OTA CHECK.";
        } else {
            enqueue = OTA_SMS_CONFIRM;
            remote_status = "Confirmation consumed. Installation has not started.";
            snprintf(detail, sizeof(detail), "Confirmed v%" PRIu32
                     ". Starting download. SMS may be temporarily unavailable.", confirmed.sequence);
            reply = detail;
        }
    } else {
        reply = "Use OTA CHECK, OTA CONFIRM <code>, OTA CANCEL, or OTA STATUS.";
    }

    if (sms_control_send(modem, sms->owner, reply) != ESP_OK) {
        ESP_LOGW(TAG, "SMS OTA command reply failed");
        if (enqueue != OTA_SMS_NONE) {
            confirmed = (ota_manifest_t){0};
            remote_status = "OTA request stopped after an SMS reply failure.";
        }
    } else if (enqueue != OTA_SMS_NONE) {
        queued = enqueue;
        queued_until = expires;
        remote_status = enqueue == OTA_SMS_CHECK ? "OTA check queued." : "OTA installation queued.";
    }
    return true;
}

static void select_remote(sms_control_t *sms, modem_t *modem)
{
    staged.install = false;
    nvs_handle_t store;
    if (update_ready(&staged) &&
        ota_notification_save(&staged.release, staged.target->address, sms->owner, &store) == ESP_OK) {
        esp_err_t result = esp_ota_set_boot_partition(staged.target);
        if (result != ESP_OK && ota_notification_clear(store) != ESP_OK) {
            ESP_LOGW(TAG, "Could not clear the boot report after failed selection");
        }
        nvs_close(store);
        if (result == ESP_OK) {
            esp_restart();
            return;
        }
    }
    sms_control_resume(sms, modem);
    remote_status = "OTA selection failed. Installation is unconfirmed.";
    if (sms->configured) {
        sms_control_send(modem, sms->owner, remote_status);
    }
}

static void finish_remote(sms_control_t *sms, modem_t *modem, update_t *update,
                          bool ready, esp_err_t result)
{
    char reply[SMS_TEXT_CAPACITY];
    uint32_t current;
    if (!update->install && ready && ota_sequence_parse(esp_app_get_description()->version, &current)) {
        if (update->release.sequence <= current) {
            remote_status = "No newer signed firmware.";
            snprintf(reply, sizeof(reply), "Current v%" PRIu32 ". Latest signed firmware is v%" PRIu32
                     ". No newer update.", current, update->release.sequence);
        } else if (create_challenge(&update->release)) {
            remote_status = "Confirmation offered. Codes expire after five minutes.";
            snprintf(reply, sizeof(reply), "Current v%" PRIu32 ". Signed update v%" PRIu32
                     " available. Reply OTA CONFIRM %s within 5 minutes or OTA CANCEL.",
                     current, update->release.sequence, challenge.code);
        } else {
            remote_status = "OTA check failed to generate a confirmation code.";
            snprintf(reply, sizeof(reply), "%s", remote_status);
        }
    } else if (update->install && ready && update_ready(update)) {
        staged = *update;
        staged_until = esp_timer_get_time() + OTA_SMS_LIFETIME_US;
        remote_status = "Image verified. Waiting for SMS service before reboot.";
        return;
    } else {
        remote_status = update->install ? "OTA install failed before boot selection."
                        : result == ESP_ERR_NOT_FOUND ? "No latest release published."
                        : "OTA check failed. Signed release or LTE unavailable.";
        snprintf(reply, sizeof(reply), "%s", remote_status);
    }

    if (!sms->configured || sms_control_send(modem, sms->owner, reply) != ESP_OK) {
        challenge = (ota_sms_challenge_t){0};
        if (!update->install) {
            remote_status = "OTA check response could not be sent.";
        }
        ESP_LOGW(TAG, "SMS OTA response submission failed");
    }
    mbedtls_platform_zeroize(reply, sizeof(reply));
}

#endif

static modem_t *execute_update(modem_t *modem, const modem_config_t *config,
                               sms_control_t *sms, update_t *update)
{
    bool reopened;
    esp_err_t result;
    update->staging_deadline = esp_timer_get_time() + STAGING_DEADLINE_US;
    modem = lte_session_run(modem, config, &reopened, update_over_ppp, update, &result);
    if (!modem) {
        ESP_LOGE(TAG, "Modem recovery failed; boot selection unchanged");
        return NULL;
    }
    bool gps_ready = false;
    if (reopened
#if defined(CONFIG_ROADHOG_OTA_SMS_BENCH)
        || update->remote
#endif
    ) {
        sms_control_resume(sms, modem);
        gps_ready = start_gps(modem);
    }
    bool ready = reopened && sms->configured && gps_ready && result == ESP_OK;
#if defined(CONFIG_ROADHOG_OTA_SMS_BENCH)
    if (update->remote) {
        finish_remote(sms, modem, update, ready, result);
        return modem;
    }
#endif
    if (!ready) {
        ESP_LOGE(TAG, "Update stopped (%s, SMS ready=%d, GNSS ready=%d); boot selection unchanged",
                 esp_err_to_name(result), sms->configured, gps_ready);
    } else if (update->install) {
        // Recheck after network teardown before changing OTA metadata.
        result = update_ready(update) ? esp_ota_set_boot_partition(update->target) : ESP_ERR_INVALID_STATE;
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "Selected %s; reboot the ESP32 manually to validate it", update->target->label);
        } else {
            ESP_LOGE(TAG, "Could not select the candidate: %s", esp_err_to_name(result));
        }
    } else {
        ESP_LOGI(TAG, "Dry run passed; SMS restored and no image staged");
    }
    return modem;
}

#if defined(CONFIG_ROADHOG_OTA_SMS_BENCH)
static modem_t *run_remote(modem_t *modem, const modem_config_t *config, sms_control_t *sms)
{
    static update_t update;
    bool install = queued == OTA_SMS_CONFIRM;
    queued = OTA_SMS_NONE;
    update = (update_t){
        .remote = true, .install = install,
        .sequence = install ? confirmed.sequence : 0,
        .release = confirmed,
    };
    confirmed = (ota_manifest_t){0};
    if (!sms->configured || !sms->owner || !update_ready(&update) ||
        esp_timer_get_time() >= queued_until) {
        sms_control_resume(sms, modem);
        remote_status = "OTA request failed preflight.";
        if (sms->configured && sms->owner) {
            sms_control_send(modem, sms->owner, remote_status);
        }
        return modem;
    }
    return execute_update(modem, config, sms, &update);
}
#endif

static modem_t *run_update(modem_t *modem, const modem_config_t *config, sms_control_t *sms,
                           const char *line)
{
    const char check[] = "ota check ";
    const char install[] = "ota install powered ";
    static update_t update;
    const char *sequence;
    if (!strncmp(line, check, sizeof(check) - 1)) {
        sequence = line + sizeof(check) - 1;
        update.install = false;
    } else if (!strncmp(line, install, sizeof(install) - 1)) {
        sequence = line + sizeof(install) - 1;
        update.install = true;
    } else {
        return modem;
    }
    if (!sms->configured || !ota_sequence_parse(sequence, &update.sequence) || !update_ready(&update)) {
        ESP_LOGW(TAG, "Update requires initialized SMS, selected VALID firmware, expected layout and a newer sequence");
        return modem;
    }

#if defined(CONFIG_ROADHOG_OTA_SMS_BENCH)
    if (update.install) {
        challenge = (ota_sms_challenge_t){0};
    }
#endif
    return execute_update(modem, config, sms, &update);
}

modem_t *ota_update_poll(modem_t *modem, const modem_config_t *config, sms_control_t *sms)
{
#if defined(CONFIG_ROADHOG_OTA_SMS_BENCH)
    sms_control_expire_pdu(sms);
    expire_remote_work(esp_timer_get_time());
    if (queued != OTA_SMS_NONE || staged.install) {
        if (!sms->configured || sms->pending_acks || sms->expected_tpdu || sms->request_count ||
            !sms_control_quiesce(sms, modem)) {
            return modem;
        }
        if (staged.install) {
            if (esp_timer_get_time() < staged_until) {
                select_remote(sms, modem);
            } else {
                staged.install = false;
                remote_status = "Queued OTA request expired.";
                sms_control_resume(sms, modem);
            }
            return modem;
        }
        return run_remote(modem, config, sms);
    }
#endif
    static char line[64];
    static size_t used;
    static bool discarded;
    static bool console_ready;
    if (!console_ready) {
        if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) == -1) {
            return modem;
        }
        console_ready = true;
        ESP_LOGI(TAG, "Bench commands: ota check <sequence>, ota install powered <sequence>");
    }
    char byte;
    // Keep SMS service moving while waiting for a manual command.
    for (unsigned i = 0; i < sizeof(line) && read(STDIN_FILENO, &byte, 1) == 1; ++i) {
        if (byte == '\n' || byte == '\r') {
            line[used] = 0;
            bool ready = used && !discarded;
            used = 0;
            discarded = false;
            if (ready) {
                if (cellular_modem_maintenance(&modem, line, provisioning_apn())) {
                    return modem;
                }
                return run_update(modem, config, sms, line);
            }
        } else if (used == sizeof(line) - 1 || byte < ' ' || byte > '~') {
            discarded = true;
        } else if (!discarded) {
            line[used++] = byte;
        }
    }
    return modem;
}
#endif
