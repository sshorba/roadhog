#include <assert.h>
#include <stddef.h>

#include "esp_ota_ops.h"
#include "ota_boot_health.h"

static const esp_partition_t slot_a = {.subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0, .label = "ota_0", .address = 0x10000};
static const esp_partition_t slot_b = {.subtype = ESP_PARTITION_SUBTYPE_APP_OTA_1, .label = "ota_1", .address = 0x410000};
static const esp_partition_t *running;
static const esp_partition_t *selected_override;
static esp_ota_img_states_t running_state;
static esp_err_t running_status;
static esp_err_t peer_status;
static esp_err_t mark_status;
static esp_err_t select_status;
static bool fallback;
static unsigned reads, marks, rollbacks, selections, restarts;

static void reset_fixture(void)
{
    running = &slot_b;
    selected_override = NULL;
    running_state = ESP_OTA_IMG_PENDING_VERIFY;
    running_status = ESP_OK;
    peer_status = ESP_ERR_NOT_FOUND;
    mark_status = select_status = ESP_OK;
    fallback = true;
    reads = marks = rollbacks = selections = restarts = 0;
}

const esp_partition_t *esp_ota_get_running_partition(void)
{
    return running;
}

const esp_partition_t *esp_ota_get_boot_partition(void)
{
    return selected_override ? selected_override : running;
}

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    assert(type == ESP_PARTITION_TYPE_APP && subtype == slot_b.subtype && label == NULL);
    return &slot_b;
}

esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *state)
{
    reads++;
    if (partition == running) {
        *state = running_state;
        return running_status;
    }
    assert(partition == &slot_b);
    *state = ESP_OTA_IMG_INVALID;
    return peer_status;
}

bool esp_ota_check_rollback_is_possible(void)
{
    return fallback;
}

esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    marks++;
    if (mark_status == ESP_OK) {
        running_state = ESP_OTA_IMG_VALID;
    }
    return mark_status;
}

esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void)
{
    rollbacks++;
    return ESP_FAIL;
}

esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition)
{
    assert(partition == &slot_b);
    selections++;
    return select_status;
}

void esp_restart(void)
{
    restarts++;
}

int main(void)
{
    reset_fixture();
    ota_boot_health_finish(true);
#if !defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
    assert(reads == 0 && marks == 0 && rollbacks == 0);
    return 0;
#elif defined(CONFIG_ROADHOG_OTA_BENCH_REJECT_PENDING)
    assert(marks == 0 && rollbacks == 1);
#else
    assert(marks == 1 && rollbacks == 0);
#endif

    reset_fixture();
    ota_boot_health_finish(false);
    assert(marks == 0 && rollbacks == 1);

    reset_fixture();
    fallback = false;
    ota_boot_health_finish(false);
    assert(marks == 0 && rollbacks == 0);

    reset_fixture();
    running = &slot_a;
    running_state = ESP_OTA_IMG_VALID;
    ota_boot_health_finish(false);
    assert(marks == 0 && rollbacks == 0 && selections == 0);

    reset_fixture();
    running_status = ESP_ERR_NOT_SUPPORTED;
    ota_boot_health_finish(true);
    assert(marks == 0 && rollbacks == 0 && selections == 0);

    reset_fixture();
    running_status = ESP_FAIL;
    ota_boot_health_finish(true);
    assert(marks == 0 && rollbacks == 0 && selections == 0);

#if defined(CONFIG_ROADHOG_OTA_BENCH_BOOT_PEER)
    reset_fixture();
    running = &slot_a;
    mark_status = ESP_FAIL;
    ota_boot_health_finish(true);
    assert(marks == 1 && selections == 0 && restarts == 0);

    reset_fixture();
    running = &slot_a;
    running_state = ESP_OTA_IMG_VALID;
    ota_boot_health_finish(true);
    assert(marks == 0 && selections == 1 && restarts == 1);

    reset_fixture();
    running = &slot_a;
    running_status = ESP_ERR_NOT_FOUND;
    ota_boot_health_finish(true);
    assert(marks == 0 && selections == 0 && restarts == 0);

    reset_fixture();
    running = &slot_a;
    running_state = ESP_OTA_IMG_VALID;
    selected_override = &slot_b;
    ota_boot_health_finish(true);
    assert(selections == 0 && restarts == 0);

    // Returning to A after B was invalidated must not select B again.
    reset_fixture();
    running = &slot_a;
    running_state = ESP_OTA_IMG_VALID;
    peer_status = ESP_OK;
    ota_boot_health_finish(true);
    assert(selections == 0 && restarts == 0);

    reset_fixture();
    running = &slot_a;
    running_state = ESP_OTA_IMG_VALID;
    select_status = ESP_FAIL;
    ota_boot_health_finish(true);
    assert(selections == 1 && restarts == 0);
#endif
}
