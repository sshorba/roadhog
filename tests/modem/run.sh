#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sed '/^#include "/d' "$root/components/modem/include/modem.h" > "$tmp/modem_header.inc"
sed '/^#include "/d' "$root/components/modem/modem.c" > "$tmp/modem_source.inc"
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -ffunction-sections -Wl,--gc-sections \
    -I "$tmp" "$root/tests/modem/test_startup_sync.c" -o "$tmp/test-startup-sync"
"$tmp/test-startup-sync"
