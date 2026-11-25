#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -r "$tmp"' EXIT
sed -n '/^static bool local_host/,/^enum { DNS_HEADER/{ /^enum { DNS_HEADER/!p; }' \
    "$root/main/portal.c" > "$tmp/handlers.inc"
sed -n '/^static esp_err_t http_open/,/^static esp_err_t start_services/{ /^static esp_err_t start_services/!p; }' \
    "$root/main/portal.c" > "$tmp/events.inc"
for ipv6 in 0 1; do
    cc -std=gnu11 -Wall -Wextra -Werror -DCONFIG_LWIP_IPV6="$ipv6" -I "$tmp" \
        "$root/tests/portal/test_http.c" -o "$tmp/portal"
    "$tmp/portal"
done
node "$root/tests/portal/test_users_refresh.js"
