#include "ota_manifest.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define HEADER "roadhog-release-v1\ntarget=esp32s3-n16r8-ota-ab-v1\n"
#define HASH "sha256=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n"
#define NOTES "notes=https://github.com/sshorba/roadhog/releases/tag/v2\n"
#define VALID HEADER "sequence=2\nsize=916000\n" HASH NOTES

int main(void)
{
    ota_manifest_t manifest;
    assert(ota_manifest_parse((const uint8_t *)VALID, sizeof(VALID) - 1, &manifest));
    assert(manifest.sequence == 2 && manifest.size == 916000);
    for (unsigned i = 0; i < sizeof(manifest.digest); ++i) {
        assert(manifest.digest[i] == i);
    }
    const char *invalid[] = {
        HEADER "sequence=02\nsize=916000\n" HASH NOTES,
        HEADER "sequence=4294967296\nsize=916000\n" HASH NOTES,
        HEADER "sequence=0\nsize=916000\n" HASH NOTES,
        HEADER "sequence=2\nsize=4194305\n" HASH NOTES,
        HEADER "sequence=2\nsequence=3\nsize=916000\n" HASH NOTES,
        HEADER "sequence=2\r\nsize=916000\n" HASH NOTES,
        HEADER "sequence=2\nsize=916000\nsha256=zz\n" NOTES,
        HEADER "sequence=2\nsize=916000\n" HASH "notes=http://github.com/test\n",
        "roadhog-release-v1\ntarget=esp32s3-2mb\nsequence=2\nsize=916000\n" HASH NOTES,
        VALID "extra=item\n",
        VALID "\n",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        assert(!ota_manifest_parse((const uint8_t *)invalid[i], strlen(invalid[i]), &manifest));
    }
    assert(!ota_manifest_parse((const uint8_t *)VALID, sizeof(VALID) - 2, &manifest));
    assert(!ota_manifest_parse((const uint8_t *)VALID, sizeof(VALID), &manifest));
    char host[254];
    const char *path;
    assert(ota_https_url("https://release-assets.githubusercontent.com/test?token=x", host, &path));
    assert(!strcmp(path, "/test?token=x"));
    assert(ota_host_allowed(host, "github.com,release-assets.githubusercontent.com"));
    assert(!ota_host_allowed("github.com.evil", "github.com,release-assets.githubusercontent.com"));
    assert(!ota_https_url("https://github.com@evil.example/test", host, &path));
    assert(!ota_https_url("https://github.com:443/test", host, &path));
    assert(!ota_https_url("https://github.com/test\r\n", host, &path));
    assert(!ota_https_url("http://github.com/test", host, &path));
    puts("OTA manifest and HTTPS URL policy passed");
}
