#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -r "$tmp"' EXIT
# ESP-IDF APIs are supplied by the NVS fixture. Compile the production logic.
sed '/^#include "/d' "$root/main/setup_button.c" > "$tmp/setup_button_under_test.inc"
sed '/^#include "/d' "$root/main/provisioning.c" > "$tmp/provisioning_under_test.inc"
sed '/^#include "/d' "$root/main/owner_verification.c" > "$tmp/owner_verification_under_test.inc"
sed -n '/^bool sms_number_valid/,/^bool sms_users_load/{ /^bool sms_users_load/!p; }
        /^static const char \*change_user/,/^static void gps_reply/{ /^static void gps_reply/!p; }' \
    "$root/main/sms_commands.c" > "$tmp/sms_authorization_under_test.inc"
sed -n '/^static const char hex\[\]/p;
        /^static bool command_ok/,/^#ifdef CONFIG_ROADHOG_SMS_CONTROL/{ /^#ifdef/!p; }
        /^static esp_err_t send_reply/,/^#ifdef CONFIG_ROADHOG_SMS_CONTROL/{ /^#ifdef/!p; }' \
    "$root/main/sms_control.c" > "$tmp/sms_delivery_under_test.inc"
sed -n '/^void sms_command_reply/,$p' "$root/main/sms_commands.c" > "$tmp/sms_commands_under_test.inc"
cc -std=gnu11 -Wall -Wextra -Werror -I "$root/main" -I "$root/tests/provisioning" -I "$tmp" \
    "$root/tests/provisioning/test_provisioning.c" -o "$tmp/provisioning"
"$tmp/provisioning"
cc -std=c11 -Wall -Wextra -Werror -I "$root/main" \
    "$root/main/sim_number.c" "$root/tests/provisioning/test_sim_number.c" -o "$tmp/sim_number"
"$tmp/sim_number"
