#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -r "$tmp"' EXIT
cc -std=c11 -Wall -Wextra -Werror -I "$root/main" \
    "$root/main/ota_manifest.c" "$root/tests/ota_release/test_manifest.c" -o "$tmp/test_manifest"
"$tmp/test_manifest"

# Exercise the production stream policy with an in-memory HTTP reader.
sed -n '/^typedef esp_err_t (\*consume_t)/,/^} body_t;/p;
        /^static bool transfer_ready(/,/^static esp_err_t download(/{ /^static esp_err_t download(/!p; }' \
    "$root/main/ota_update.c" > "$tmp/transfer_functions.inc"
cc -std=c11 -Wall -Wextra -Werror -I "$tmp" \
    "$root/tests/ota_release/test_transfer.c" -o "$tmp/test_transfer"
"$tmp/test_transfer"

python3 - "$root" "$tmp" <<'PY'
import hashlib
from pathlib import Path
import struct
import subprocess
import sys

root, tmp = map(Path, sys.argv[1:])
image_name = tmp / "roadhog-v2.bin"
image = bytearray(512)
image[0] = 0xE9
image[3] = 0x40
struct.pack_into("<H", image, 12, 9)
struct.pack_into("<I", image, 32, 0xABCD5432)
image[48] = ord("2")
image[80:87] = b"roadhog"
image_name.write_bytes(image)
notes = "https://github.com/sshorba/roadhog/releases/tag/v2"
subprocess.run([sys.executable, root / "tools/ota_release_manifest.py", image_name, "2", notes], check=True)
manifest = image_name.with_suffix(".manifest")
assert manifest.read_bytes() == (
    "roadhog-release-v1\ntarget=esp32s3-n16r8-ota-ab-v1\n"
    f"sequence=2\nsize=512\nsha256={hashlib.sha256(image).hexdigest()}\nnotes={notes}\n"
).encode("ascii")
PY
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$tmp/private.pem"
openssl pkey -in "$tmp/private.pem" -pubout -out "$tmp/public.pem"
openssl dgst -sha256 -sign "$tmp/private.pem" -out "$tmp/test.sig" "$tmp/roadhog-v2.manifest"
openssl dgst -sha256 -verify "$tmp/public.pem" -signature "$tmp/test.sig" "$tmp/roadhog-v2.manifest"
printf '\n' >> "$tmp/roadhog-v2.manifest"
if openssl dgst -sha256 -verify "$tmp/public.pem" -signature "$tmp/test.sig" "$tmp/roadhog-v2.manifest" >/dev/null 2>&1; then
    exit 1
fi
echo "Canonical release signing and tamper fixture passed"
