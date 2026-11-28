#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define OTA_SLOT(subtype) ((subtype) & 0x0f)

enum { FACTORY_INDEX = -1, INVALID_INDEX = -99, FLASH_SECTOR_SIZE = 4096,
       ESP_IMAGE_VERIFY, ESP_IMAGE_VERIFY_SILENT };
enum { slot_a, slot_b, slot_count };

typedef struct { uint32_t offset, size; } esp_partition_pos_t;
typedef struct {
    esp_partition_pos_t ota_info, factory;
    unsigned app_count;
} bootloader_state_t;
typedef struct {
    uint32_t ota_seq;
    uint8_t seq_label[20];
    uint32_t ota_state, crc;
} esp_ota_select_entry_t;

static const char *TAG = "ota-sdk-test";
static const esp_partition_t apps[slot_count] = {
    {ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0", 0x10000, 0x400000, FLASH_SECTOR_SIZE},
    {ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, "ota_1", 0x410000, 0x400000, FLASH_SECTOR_SIZE},
};
static const esp_partition_t metadata = {
    ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata", 0x810000,
    2 * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE,
};
static const bootloader_state_t layout = {
    .ota_info = {0x810000, 2 * FLASH_SECTOR_SIZE},
    .app_count = slot_count,
};
static uint8_t sectors[2 * FLASH_SECTOR_SIZE];
static const esp_partition_t *running;
static bool ota_has_initial_contents;
static bool bootloader_write_fails;
static bool metadata_erase_fails, metadata_write_fails;
static unsigned restarts;

#include "ota_boot_health.h"
ota_health_result_t selector_finish(bool healthy, bool expired);
ota_health_result_t reject_finish(bool healthy, bool expired);

const esp_partition_t *esp_ota_get_running_partition(void)
{
    return running;
}

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    assert(label == NULL);
    for (unsigned i = 0; i < slot_count; i++) {
        if (apps[i].type == type && apps[i].subtype == subtype) {
            return &apps[i];
        }
    }
    return type == metadata.type && subtype == metadata.subtype ? &metadata : NULL;
}

static bool is_ota_partition(const esp_partition_t *partition)
{
    return partition == &apps[slot_a] || partition == &apps[slot_b];
}

static const esp_partition_t *find_default_boot_partition(void)
{
    return &apps[slot_a];
}

static unsigned esp_ota_get_app_partition_count(void)
{
    return slot_count;
}

static esp_err_t image_validate(const esp_partition_t *partition, int mode)
{
    (void)mode;
    // Both staged test images are loadable. The failure is at app startup.
    return is_ota_partition(partition) ? ESP_OK : ESP_FAIL;
}

static esp_err_t bootloader_common_read_otadata(const esp_partition_pos_t *partition,
                                               esp_ota_select_entry_t entries[2])
{
    assert(partition->offset == metadata.address);
    memcpy(&entries[0], sectors, sizeof(entries[0]));
    memcpy(&entries[1], sectors + FLASH_SECTOR_SIZE, sizeof(entries[1]));
    return ESP_OK;
}

static const esp_partition_t *read_otadata(esp_ota_select_entry_t entries[2])
{
    bootloader_common_read_otadata(&layout.ota_info, entries);
    return &metadata;
}

static esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset, size_t size)
{
    assert(partition == &metadata && offset + size <= sizeof(sectors));
    if (metadata_erase_fails) {
        assert(offset == FLASH_SECTOR_SIZE);
        return ESP_FAIL;
    }
    memset(sectors + offset, 0xff, size);
    return ESP_OK;
}

static esp_err_t esp_partition_write(const esp_partition_t *partition, size_t offset,
                                     const void *entry, size_t size)
{
    assert(partition == &metadata && offset + size <= sizeof(sectors));
    if (metadata_write_fails) {
        assert(offset == FLASH_SECTOR_SIZE && size == sizeof(esp_ota_select_entry_t));
        // Simulate a torn write that leaves the candidate CRC erased.
        memcpy(sectors + offset, entry, offsetof(esp_ota_select_entry_t, crc));
        return ESP_FAIL;
    }
    memcpy(sectors + offset, entry, size);
    return ESP_OK;
}

static bool esp_efuse_is_flash_encryption_enabled(void)
{
    return false;
}

static esp_err_t write_otadata(esp_ota_select_entry_t *entry, uint32_t address, bool encrypted)
{
    assert(!encrypted);
    if (bootloader_write_fails) {
        return ESP_FAIL;
    }
    size_t offset = address - metadata.address;
    esp_partition_erase_range(&metadata, offset, FLASH_SECTOR_SIZE);
    return esp_partition_write(&metadata, offset, entry, sizeof(*entry));
}

static uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *bytes, size_t length)
{
    crc = ~crc;
    while (length--) {
        crc ^= *bytes++;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
        }
    }
    return ~crc;
}

void esp_restart(void)
{
    restarts++;
}

#include "sdk_ota_functions.inc"

static void boot_slot(int expected)
{
    int selected = bootloader_utility_get_selected_boot_partition(&layout);
    assert(selected == expected);
    // The real loader calls this only after loading a valid image.
    set_actual_ota_seq(&layout, selected);
    running = &apps[selected];
}

static void expect_state(unsigned slot, esp_ota_img_states_t expected)
{
    esp_ota_img_states_t state;
    assert(esp_ota_get_state_partition(&apps[slot], &state) == ESP_OK);
    assert(state == expected);
}

int main(void)
{
    memset(sectors, 0xff, sizeof(sectors));
    esp_ota_img_states_t state;
    assert(esp_ota_get_state_partition(&apps[slot_a], &state) == ESP_ERR_NOT_FOUND);

    // An unsuccessful metadata write can still let A execute. Never arm B.
    bootloader_write_fails = true;
    boot_slot(slot_a);
    assert(esp_ota_get_state_partition(running, &state) == ESP_ERR_NOT_FOUND);
    selector_finish(true, false);
    assert(restarts == 0);
    assert(esp_ota_get_state_partition(&apps[slot_b], &state) == ESP_ERR_NOT_FOUND);

    bootloader_write_fails = false;
    boot_slot(slot_a);
    expect_state(slot_a, ESP_OTA_IMG_VALID);
    assert(esp_ota_get_boot_partition() == running);
    selector_finish(false, false);
    assert(restarts == 0);

    uint8_t before[sizeof(sectors)];
    memcpy(before, sectors, sizeof(before));
    metadata_erase_fails = true;
    assert(esp_ota_set_boot_partition(&apps[slot_b]) == ESP_FAIL);
    assert(!memcmp(before, sectors, sizeof(sectors)));
    assert(esp_ota_get_boot_partition() == running);
    metadata_erase_fails = false;

    metadata_write_fails = true;
    assert(esp_ota_set_boot_partition(&apps[slot_b]) == ESP_FAIL);
    assert(!memcmp(before, sectors, FLASH_SECTOR_SIZE));
    assert(memcmp(before + FLASH_SECTOR_SIZE, sectors + FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE));
    assert(esp_ota_get_boot_partition() == running);
    assert(bootloader_utility_get_selected_boot_partition(&layout) == slot_a);
    expect_state(slot_a, ESP_OTA_IMG_VALID);
    metadata_write_fails = false;

    selector_finish(true, false);
    assert(restarts == 1);
    expect_state(slot_a, ESP_OTA_IMG_VALID);
    expect_state(slot_b, ESP_OTA_IMG_NEW);

    boot_slot(slot_b);
    expect_state(slot_b, ESP_OTA_IMG_PENDING_VERIFY);
    assert(esp_ota_check_rollback_is_possible());
    reject_finish(true, false);
    assert(restarts == 2);
    expect_state(slot_b, ESP_OTA_IMG_INVALID);

    boot_slot(slot_a);
    expect_state(slot_a, ESP_OTA_IMG_VALID);
    selector_finish(true, false);
    assert(restarts == 2);
    puts("ESP-IDF 6.1.1 host lifecycle: metadata failures, VALID A, rejected B, return to A");
}
