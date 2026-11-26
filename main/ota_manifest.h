#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_SLOT_SIZE 0x400000
#define OTA_MANIFEST_MAX 1024
#define OTA_URL_MAX 2048

typedef struct {
    uint32_t sequence;
    uint32_t size;
    uint8_t digest[32];
} ota_manifest_t;

bool ota_sequence_parse(const char *text, uint32_t *sequence);
bool ota_manifest_parse(const uint8_t *data, size_t size, ota_manifest_t *manifest);
bool ota_https_url(const char *url, char host[254], const char **path);
bool ota_host_allowed(const char *host, const char *allowlist);
