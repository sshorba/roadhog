#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
for mode in legacy normal reject select; do
    case $mode in
        legacy) set -- ;;
        normal) set -- -DCONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE ;;
        reject) set -- -DCONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE -DCONFIG_ROADHOG_OTA_BENCH_REJECT_PENDING ;;
        select) set -- -DCONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE -DCONFIG_ROADHOG_OTA_BENCH_BOOT_PEER ;;
    esac
    cc -std=c17 -Wall -Wextra -Werror "$@" -I tests/ota_boot_health -I main \
        main/ota_boot_health.c tests/ota_boot_health/test_boot_health.c -o "$test_dir/$mode"
    "$test_dir/$mode"
    printf '%s boot health test passed\n' "$mode"
done
