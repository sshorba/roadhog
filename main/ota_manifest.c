#include "ota_manifest.h"

#include <string.h>

static bool decimal(const char *text, size_t length, uint32_t *value)
{
    if (!length || text[0] < '1' || text[0] > '9') {
        return false;
    }
    uint32_t number = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned digit = (unsigned)(text[i] - '0');
        if (digit > 9 || number > (UINT32_MAX - digit) / 10) {
            return false;
        }
        number = number * 10 + digit;
    }
    *value = number;
    return true;
}

bool ota_sequence_parse(const char *text, uint32_t *sequence)
{
    return decimal(text, strlen(text), sequence);
}

bool ota_https_url(const char *url, char host[254], const char **path)
{
    static const char prefix[] = "https://";
    if (strncmp(url, prefix, sizeof(prefix) - 1) || strlen(url) >= OTA_URL_MAX) {
        return false;
    }
    const char *hostname = url + sizeof(prefix) - 1;
    const char *slash = strchr(hostname, '/');
    if (!slash || slash == hostname || slash - hostname >= 254 ||
        hostname[0] == '.' || slash[-1] == '.' ||
        strspn(hostname, "abcdefghijklmnopqrstuvwxyz0123456789.-") != (size_t)(slash - hostname)) {
        return false;
    }
    for (const char *p = slash; *p; ++p) {
        if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127 || *p == '#' || *p == '\\') {
            return false;
        }
    }
    memcpy(host, hostname, slash - hostname);
    host[slash - hostname] = '\0';
    *path = slash;
    return true;
}

bool ota_host_allowed(const char *host, const char *allowlist)
{
    size_t host_length = strlen(host);
    while (*allowlist) {
        size_t length = strcspn(allowlist, ",");
        if (host_length == length && !memcmp(host, allowlist, length)) {
            return true;
        }
        allowlist += length;
        if (*allowlist) {
            ++allowlist;
        }
    }
    return false;
}

static const char *field(const char **cursor, const char *prefix, size_t *length)
{
    const char *value = *cursor;
    if (strncmp(value, prefix, strlen(prefix))) {
        return NULL;
    }
    value += strlen(prefix);
    const char *end = strchr(value, '\n');
    if (!end || end == value) {
        return NULL;
    }
    *length = end - value;
    *cursor = end + 1;
    return value;
}

bool ota_manifest_parse(const uint8_t *data, size_t size, ota_manifest_t *manifest)
{
    static const char header[] = "roadhog-release-v1\n"
                                 "target=esp32s3-n16r8-ota-ab-v1\n";
    const char hex[] = "0123456789abcdef";
    if (size < sizeof(header) || size > OTA_MANIFEST_MAX ||
        memchr(data, 0, size) || memcmp(data, header, sizeof(header) - 1)) {
        return false;
    }

    char text[OTA_MANIFEST_MAX + 1];
    memcpy(text, data, size);
    text[size] = '\0';
    const char *cursor = text + sizeof(header) - 1;
    size_t length;
    const char *value = field(&cursor, "sequence=", &length);
    if (!value || !decimal(value, length, &manifest->sequence)) {
        return false;
    }
    value = field(&cursor, "size=", &length);
    if (!value || !decimal(value, length, &manifest->size) || manifest->size > OTA_SLOT_SIZE) {
        return false;
    }
    value = field(&cursor, "sha256=", &length);
    if (!value || length != sizeof(manifest->digest) * 2) {
        return false;
    }
    for (size_t i = 0; i < sizeof(manifest->digest); ++i) {
        const char *hi = strchr(hex, value[i * 2]);
        const char *lo = strchr(hex, value[i * 2 + 1]);
        if (!hi || !lo) {
            return false;
        }
        manifest->digest[i] = ((hi - hex) << 4) | (lo - hex);
    }
    // The tag and binary filename are derived from the signed sequence.
    value = field(&cursor, "notes=", &length);
    if (!value || *cursor) {
        return false;
    }
    text[value - text + length] = '\0';
    char host[254];
    const char *path;
    return ota_https_url(value, host, &path);
}
