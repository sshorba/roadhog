/* Model the SMS submission and durable journal around real loopback TLS. */
#define OWNER "+15550101001"
#define OTHER_USER "+15550101002"
static char last_reply[SMS_TEXT_CAPACITY];
static unsigned sent, restarted;
static bool fail_send, fail_commit;
static uint8_t nvs_durable[128], nvs_uncommitted[128];
static size_t durable_size, uncommitted_size;

typedef int nvs_handle_t;
enum { NVS_READONLY, NVS_READWRITE };

static esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *store)
{
    assert(!strcmp(name, "sms_ota"));
    *store = mode;
    memcpy(nvs_uncommitted, nvs_durable, durable_size);
    uncommitted_size = durable_size;
    return ESP_OK;
}

static esp_err_t nvs_get_blob(nvs_handle_t store, const char *key, void *data, size_t *size)
{
    assert((store == NVS_READONLY || store == NVS_READWRITE) && !strcmp(key, "pending_v1"));
    if (!durable_size) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    assert(*size >= durable_size);
    *size = durable_size;
    memcpy(data, nvs_durable, durable_size);
    return ESP_OK;
}

static esp_err_t nvs_set_blob(nvs_handle_t store, const char *key, const void *data, size_t size)
{
    assert(store == NVS_READWRITE && !strcmp(key, "pending_v1") && size <= sizeof(nvs_uncommitted));
    memcpy(nvs_uncommitted, data, size);
    uncommitted_size = size;
    return ESP_OK;
}

static esp_err_t nvs_erase_key(nvs_handle_t store, const char *key)
{
    assert(store == NVS_READWRITE && !strcmp(key, "pending_v1"));
    uncommitted_size = 0;
    return ESP_OK;
}

static esp_err_t nvs_commit(nvs_handle_t store)
{
    assert(store == NVS_READWRITE);
    if (fail_commit) {
        return ESP_ERR_FLASH_OP_FAIL;
    }
    memcpy(nvs_durable, nvs_uncommitted, uncommitted_size);
    durable_size = uncommitted_size;
    return ESP_OK;
}

static void nvs_close(nvs_handle_t store)
{
    assert(store == NVS_READONLY || store == NVS_READWRITE);
}

static esp_err_t sms_control_send(modem_t *modem, const char *number, const char *message)
{
    assert(modem == &fixture_modem && !ppp_active && uart_open && strlen(message) < sizeof(last_reply));
    assert(!strcmp(number, OWNER));
    strcpy(last_reply, message);
    ++sent;
    return fail_send ? ESP_FAIL : ESP_OK;
}

static void esp_restart(void)
{
    assert(selected != running && durable_size && !ppp_active && at_ready && sms_ready && gps_ready);
    ++restarted;
}

typedef int mbedtls_svc_key_id_t;
#define PSA_KEY_TYPE_HMAC 20
#define PSA_KEY_USAGE_SIGN_MESSAGE 21
#define PSA_ALG_HMAC(alg) ((alg) + 22)
static uint8_t hmac_seed[32];

static void bootloader_random_enable(void) {}
static void bootloader_random_disable(void) {}

static void esp_fill_random(void *bytes, size_t length)
{
    memset(bytes, 0x55, length);
}

static void mbedtls_platform_zeroize(void *bytes, size_t length)
{
    OPENSSL_cleanse(bytes, length);
}

static void psa_set_key_type(psa_key_attributes_t *attributes, int type)
{
    assert(!*attributes && type == PSA_KEY_TYPE_HMAC);
}

static void psa_set_key_usage_flags(psa_key_attributes_t *attributes, int flags)
{
    assert(!*attributes && flags == PSA_KEY_USAGE_SIGN_MESSAGE);
}

static void psa_set_key_algorithm(psa_key_attributes_t *attributes, int algorithm)
{
    assert(!*attributes && algorithm == PSA_ALG_HMAC(PSA_ALG_SHA_256));
}

static int psa_import_key(const psa_key_attributes_t *attributes, const uint8_t *key,
                          size_t size, mbedtls_svc_key_id_t *id)
{
    assert(!*attributes && size == sizeof(hmac_seed));
    memcpy(hmac_seed, key, size);
    *id = 1;
    return PSA_SUCCESS;
}

static int psa_mac_compute(mbedtls_svc_key_id_t id, int algorithm, const uint8_t *message,
                           size_t size, uint8_t *out, size_t capacity, size_t *out_size)
{
    assert(id == 1 && algorithm == PSA_ALG_HMAC(PSA_ALG_SHA_256) && capacity >= 32);
    unsigned length;
    assert(HMAC(EVP_sha256(), hmac_seed, sizeof(hmac_seed), message, size, out, &length));
    *out_size = length;
    return PSA_SUCCESS;
}

#define CONFIG_ROADHOG_SMS_CONTROL 1
enum { MAX_PDU_BYTES = 190 };
static const char hex[] = "0123456789ABCDEF";
static const char *SMS_TAG = "sms";
static unsigned acks;
static bool fail_ack;

static bool command_ok(modem_t *modem, const char *command)
{
    assert(modem == &fixture_modem && !ppp_active && !strcmp(command, "AT+CNMA"));
    ++acks;
    return !fail_ack;
}

static bool sms_sender_allowed(const sms_control_t *sms, const char *sender)
{
    return !strcmp(sms->owner, sender) || !strcmp(sender, OTHER_USER);
}

static void sms_command_reply(sms_control_t *sms, modem_t *modem, const sms_request_t *request,
                              char reply[SMS_TEXT_CAPACITY])
{
    (void)sms;
    (void)modem;
    (void)request;
    (void)reply;
    assert(!"Unexpected generic command");
}
