#!/bin/sh
set -eu
component=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
root=$(dirname "$(dirname "$component")")
modem_include=${1:-"$root/managed_components/espressif__esp_modem/include"}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
awk '
/^static bool parse_number/ { emit=1 }
/^static bool recover_at/ { emit=0 }
/^struct cid_owner_t/ { emit=1 }
/^#ifdef CONFIG_ROADHOG_MODEM_MAINTENANCE_BENCH/ { emit=0 }
emit { print }
' "$component/cellular_net.cpp" > "$tmp/cid8.inc"
awk '
/^#ifdef CONFIG_ROADHOG_MODEM_MAINTENANCE_BENCH/ { emit=1 }
/^bool cellular_ppp_is_uplink/ { emit=0 }
emit { print }
' "$component/cellular_net.cpp" > "$tmp/maintenance.inc"
"${CXX:-c++}" -DCONFIG_ROADHOG_MODEM_MAINTENANCE_BENCH -std=c++20 -Wall -Wextra -Werror -I "$tmp" -I "$modem_include" \
    "$component/tests/cid8_test.cpp" -lcrypto -o "$tmp/cid8-test"
"$tmp/cid8-test"
