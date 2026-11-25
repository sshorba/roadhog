# First attended LTE OTA preflight

This run ends after `ota check 2` and AT/SMS/GNSS recovery. Installer
provisioning, a maintenance-console session, reboot and any SMS test each
require the owner's approval before they happen. The local hardware agent is
used for passive observation. [Signed release format](github-releases-ota.md)

**Bench only, default off.** With Secure Boot disabled, bootloader fallback
may load unverified inactive-slot bytes if the selected app fails to load.
This remains unresolved even when application-level OTA verification reports
failure and leaves boot selection unchanged. Keep OTA disabled for unattended
deployments, fielded vehicles and assembled units. [Trust and rollback](github-releases-ota.md#trust-and-rollback)

## Identify the baseline before choosing an installer target

Use the most recent verified boot and readback as historical evidence. The
first signed LTE session independently verified v2 in B before boot, then
observed B become VALID and preserved A as fallback. A passive station
report can show UART activity but does not identify the live image, slot or
configuration. [Signed bench report](https://github.com/sshorba/roadhog/pull/10#issuecomment-7171430168)

1. Use existing local boot logs and the most recent accepted flash or OTA
   record before doing anything with the device. Record the latest booted
   partition and its state, the selected next boot partition, project version,
   app ELF SHA-256, full application binary SHA-256, source commit, and the
   matching saved `sdkconfig`. Confirm no later write or boot selection
   invalidated that record. The live log and build record must agree.
2. Inspect the saved app binary and config offline. The ELF SHA in the app
   descriptor and the SHA-256 of the complete application binary are
   distinct values.

   ```sh
   python -m esptool --chip esp32s3 image-info "$baseline_image"
   sha256sum "$baseline_image" "$baseline_sdkconfig"
   grep -E '^CONFIG_ROADHOG_OTA_UPDATE_BENCH=y$' "$baseline_sdkconfig"
   ```

3. If records are missing or disagree, stop. At an approved maintenance window,
   the operator can collect boot evidence and obtain a fresh ROM **readback**
   of the partition table, OTA metadata and app slots using the local station.
   Compare the app bytes through the exact saved binary length against the
   saved artifact and retain the full-slot backup separately. The booted slot
   comes from the boot log, while the two OTA records establish which slot is
   selected and VALID. Metadata alone cannot prove which image was running
   before entry into ROM. Keep any backup and logs with private configuration
   offline. See the [layout and backup procedure](ota-migration.md).
4. A one-time approved installer provision is required if the verified app
   lacks the bench option or cannot accept the bench key, endpoint or sequence.
   The first trial needs a selected VALID installer with decimal version
   `1`, the matching bench public key and the chosen HTTPS base and hosts.
   Reuse an installer that already meets those conditions. Make the decision
   using the current verified baseline.

## Prepare an installer and candidate offline

Use a clean checkout of the reviewed firmware revision and ESP-IDF v6.1.1.
Use an empty private staging directory and new config files. Reserve a
temporary HTTPS hostname and allowlist before building. Replace the sample
host and enter a fresh random path. If testing a redirect, add its exact
hostname to the allowlist too. Keep TLS certificate bundle and SNI enabled.

```sh
set -eu
umask 077
test -z "$(git status --porcelain)"
stage=$(mktemp -d)
git rev-parse HEAD > "$stage/source-commit"
idf.py --version > "$stage/idf-version"
openssl ecparam -name prime256v1 -genkey -noout -out "$stage/bench-private.pem"
openssl pkey -in "$stage/bench-private.pem" -pubout -out "$stage/bench-public.pem"

bench_host=bench.example.net
bench_hosts="$bench_host"
# For a redirect trial, use bench_hosts="$bench_host,redirect.example.net".
bench_path=$(openssl rand -hex 16)
bench_base="https://$bench_host/$bench_path"
cat > "$stage/bench.defaults" <<CONFIG
CONFIG_ROADHOG_OTA_UPDATE_BENCH=y
CONFIG_ROADHOG_OTA_RELEASE_BASE="$bench_base"
CONFIG_ROADHOG_OTA_RELEASE_HOSTS="$bench_hosts"
CONFIG

for version in 1 2; do
  idf.py -B "$stage/build-v$version" -D "SDKCONFIG=$stage/v$version.sdkconfig" \
    -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.ota-16mb.defaults;sdkconfig.lte-diagnostic.defaults;$stage/bench.defaults" \
    -D "PROJECT_VER=$version" -D "ROADHOG_OTA_PUBLIC_KEY=$stage/bench-public.pem" build
  grep -Fx CONFIG_ROADHOG_OTA_UPDATE_BENCH=y "$stage/v$version.sdkconfig"
  grep -Fx CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y "$stage/v$version.sdkconfig"
  grep -Fx CONFIG_MBEDTLS_SSL_SERVER_NAME_INDICATION=y "$stage/v$version.sdkconfig"
  grep -Fx '# CONFIG_ROADHOG_OTA_BENCH_BOOT_PEER is not set' "$stage/v$version.sdkconfig"
  grep -Fx '# CONFIG_ROADHOG_OTA_BENCH_REJECT_PENDING is not set' "$stage/v$version.sdkconfig"
  python -m esptool --chip esp32s3 image-info "$stage/build-v$version/roadhog.bin"
  cp "$stage/build-v$version/roadhog.bin" "$stage/roadhog-v$version.bin"
done

python3 tools/ota_release_manifest.py "$stage/roadhog-v2.bin" 2 "$bench_base/v2"
openssl dgst -sha256 -sign "$stage/bench-private.pem" \
  -out "$stage/roadhog-v2.sig" "$stage/roadhog-v2.manifest"
openssl dgst -sha256 -verify "$stage/bench-public.pem" \
  -signature "$stage/roadhog-v2.sig" "$stage/roadhog-v2.manifest"
(cd "$stage" && sha256sum source-commit idf-version bench.defaults bench-public.pem \
  v1.sdkconfig v2.sdkconfig roadhog-v1.bin roadhog-v2.bin roadhog-v2.manifest \
  roadhog-v2.sig build-v1/partition_table/partition-table.bin > SHA256SUMS)
(cd "$stage" && sha256sum -c SHA256SUMS)
```

Record both successful build summaries, both versions and slot fit, and the
compiler/SDK versions alongside the checksum file. Keep the private key on
the signing machine. Transfer just the three v2 files to the temporary host.
Hash the files after copying. The bench key is disposable and specific to
this trial. Retire the bench installer or move to a separately approved
production trust anchor before discarding the key. Do not sign production
releases with it.

## Temporary HTTPS endpoint

Use a short-lived HTTPS VM or existing HTTPS virtual host for the attended
window. A standalone Caddy static site is sufficient. Use a public DNS A
record, port 443 and a hostname certificate whose chain is trusted by the
ESP-IDF common bundle. Serve only the three signed v2 assets under
`/<random-path>/v2/`. For example, Caddy's site block can use the following
on a disposable host.

```caddyfile
bench.example.net {
    root * /srv/roadhog-bench
    file_server
    log
}
```

Copy only the three test files into
`/srv/roadhog-bench/<random-path>/v2/`, with directory listing disabled.
The random path limits discovery. The endpoint is Internet-reachable, so
serve only generic, non-secret artifacts. From a separate client verify the
three exact URLs with normal CA and hostname validation and compare the
downloaded bytes with `SHA256SUMS`. Retain only sanitized status logs.
For the two-host redirect trial, reserve a second DNS name and certificate.
Serve the same asset tree on that host and replace the first host's
`file_server` with `redir https://redirect.example.net{uri} 302`. Confirm both
names were included in the installer allowlist before building. In the HTTPS
logs compare `request.tls.server_name` with `request.host` for each hop.
Remove the files and tear down the host or route immediately after the trial.

## Approved installer provision and dry run

If provisioning is required, first confirm physical identity, stable modem
and ESP32 power, the exact running and selected VALID slot, and a fresh
verified backup. Confirm the installer checksum, the bootloader's rollback
support, and the actual A/B table. Stop the agent and close other serial
clients during any approved hands-on operation.

The simple recovery plan for a confirmed selected-VALID baseline is an
owner-approved local **app-only** replacement of that slot with v1. For a
freshly confirmed A baseline use offset `0x10000`. For a confirmed B
baseline use `0x410000`. Each slot is `0x400000` bytes. The operator must
verify the actual offset and image digest before writing. Preserve the peer
slot, bootloader, partition table, NVS, and OTA metadata. Read back the
written application bytes and compare the installer digest before any
approved boot. This direct replacement of a VALID slot does not exercise
automatic rollback. Keep the original slot image and verified backup ready
for a separately authorized local restore if the installer fails. Stop if
the backup, selection, layout or recovery route cannot be verified.

After approved startup, confirm version 1, selected VALID state and generic
provisioning before any LTE command. In an approved attended console
session, attach to the confirmed ESP32 console at its configured baud rate.
The local operator enters this line and presses Enter.

```text
ota check 2
```

Expect PPP IPv4, signed v2 manifest verification, command-mode recovery, SMS
configuration and GNSS recovery, and a dry-run success message. This command
fetches the manifest and signature. It stages no app bytes and leaves the
selected boot slot unchanged. A certificate, routing, signature or recovery
failure ends the session. Record observations locally and redact them before
posting. Resume the passive agent after releasing the console. Stop here
before considering any install or reboot of a candidate.

## ESP-IDF v6.1.1 TLS check

Inspected the actual SDK tag `v6.1.1` at
`051e4c4eb20346e043cdd47bc388fde75ff8af63` and its Mbed TLS checkout
`ce3f3485a121c100f58f36d700cb35b060f6e866`. In the compiled installer,
`CONFIG_ESP_TLS_USING_MBEDTLS`, `CONFIG_MBEDTLS_SSL_SERVER_NAME_INDICATION`
and the certificate bundle are enabled. The ELF links the SSL transport
common-name setter, `mbedtls_ssl_set_hostname` and X.509 verification.

- `esp_http_client_init()` passes `common_name` through
  `esp_transport_ssl_set_common_name()` to ESP-TLS's `cfg.common_name`.
  The transport connects using the URL host (the bound PPP connection's
  resolved IPv4 address). The HTTP `Host` header is set separately.
- ESP-TLS `set_client_config()` prefers that override to the socket host
  and passes it to `mbedtls_ssl_set_hostname()`. Attaching the certificate
  bundle sets `MBEDTLS_SSL_VERIFY_REQUIRED`. The bundled verifier passes
  the stored hostname into X.509 verification. The bundle callback preserves
  hostname mismatch errors.
- The SDK's Mbed TLS ClientHello calls `ssl_write_hostname_ext()` when SNI
  is enabled and copies that same stored hostname into the extension.
  `download()` disables automatic redirects and creates a fresh client for
  each allowed hop, with a newly resolved IPv4 address and the new hop's
  hostname for both TLS and HTTP. No hostname fix is indicated by this audit.

This verifies the tagged SDK's implementation and target build configuration.
The first private trial established live PPP and retrieved the signed
manifest and image over the authenticated HTTPS path. The two-hop SNI
comparison and deliberate hostname rejection remain to be exercised on
ESP32. In approved dry runs, compare the server's TLS server name and HTTP
Host at each allowlisted hop, then require a certificate valid only for a
different name to fail before staging or boot selection. The libcurl
fixture checks these cases offline.
