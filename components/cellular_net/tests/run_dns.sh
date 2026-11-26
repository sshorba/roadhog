#!/bin/sh
set -eu
dir=$(CDPATH='' cd -- "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sed -n '/^struct dns_lookup_t {/,/^static esp_err_t prepare_tls/{ /^static esp_err_t prepare_tls/!p; }' \
    "$dir/../cellular_net.cpp" > "$tmp/dns_source.inc"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$tmp" "$dir/dns_test.cpp" -o "$tmp/test-dns"
"$tmp/test-dns"
