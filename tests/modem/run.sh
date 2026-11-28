#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sed '/^#include "/d' "$root/components/modem/include/modem.h" > "$tmp/modem_header.inc"
sed '/^#include "/d' "$root/components/modem/modem.c" > "$tmp/modem_source.inc"
sed -n '/^static SemaphoreHandle_t worker_done;/,/^modem_t \*lte_session_run/{ /^modem_t \*lte_session_run/!p; }' \
    "$root/main/lte_diagnostic.c" > "$tmp/lte_uart_ownership.inc"
sed '/^#include "/d' "$root/main/connection_status.h" > "$tmp/connection_status_header.inc"
sed '/^#include "/d' "$root/main/connection_status.c" > "$tmp/connection_status_source.inc"
sed -n '/^static modem_final_t probe(/,/^static void cache_sim_number(/{ /^static void cache_sim_number(/!p; }
        /^static bool wait_for_at_boundary(/,/^void app_main(/{ /^void app_main(/!p; }' \
    "$root/main/main.c" > "$tmp/online_status.inc"
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -ffunction-sections -Wl,--gc-sections \
    -DCONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1 \
    -I "$tmp" -I "$root/main" -I "$root/tests/ota_boot_health" \
    "$root/tests/modem/test_startup_sync.c" "$root/main/ota_boot_health.c" "$root/main/modem_lifecycle.c" -o "$tmp/test-startup-sync"
"$tmp/test-startup-sync"
