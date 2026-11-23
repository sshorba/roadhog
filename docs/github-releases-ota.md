# Bench OTA over LTE

The bench installer builds for ESP-IDF v6.1.1 and the migrated ESP32-S3 N16R8
layout. The 4 MiB application slots are at `0x10000` and `0x410000`. OTA metadata
is at `0x810000`. The device must already be running the selected, VALID
application on the rollback-capable bootloader. See the [migration guide](ota-migration.md).

## Release format

Use a fresh, increasing decimal release sequence `N` from 1 through 4294967295,
with no leading zero. Build with `-D PROJECT_VER=N`. The release tag is `vN`.
Publish release notes and the following assets under that tag.

- `roadhog-vN.bin` is the exact ESP-IDF application image.
- `roadhog-vN.manifest` authenticates the sequence and image.
- `roadhog-vN.sig` is the detached ASN.1 DER ECDSA P-256/SHA-256 signature.

The manifest is ASCII, with exactly these ordered fields and LF line endings,
including the final LF. The target identifies Road Hög on ESP32-S3 N16R8 with
16 MiB flash and the v1 4 MiB A/B layout. Asset names are derived from the
signed sequence. Unknown fields, duplicate fields, alternate number encodings,
embedded NULs, overlong metadata and unexpected targets are rejected. The
signature covers the exact file bytes and is verified before manifest parsing.

```text
roadhog-release-v1
target=esp32s3-n16r8-ota-ab-v1
sequence=<N>
size=<exact byte count>
sha256=<64 lowercase hexadecimal digits>
notes=<absolute HTTPS URL for release notes>
```

After a release build has been reviewed, copy only its app binary into a
private staging directory. Use an offline signing system and transfer only
the three release assets out.

```sh
python3 tools/ota_release_manifest.py /path/to/roadhog-v2.bin 2 \
  https://github.com/sshorba/roadhog/releases/tag/v2
openssl dgst -sha256 -sign /offline/release-private.pem \
  -out /path/to/roadhog-v2.sig /path/to/roadhog-v2.manifest
openssl dgst -sha256 -verify /path/to/release-public.pem \
  -signature /path/to/roadhog-v2.sig /path/to/roadhog-v2.manifest
```

Use a PEM SubjectPublicKeyInfo P-256 public key. Keep the private key offline
and out of source control, CI, and release assets. Review the image provenance,
test results, embedded public key, configuration and asset checksums before any
publication. A personalized test binary contains its compiled SMS owner.
**Keep personalized binaries on a private bench endpoint.** Generic production
publishing depends on the reviewed owner-provisioning work described in the
[design notes](github-releases-ota-design.md).

The bench operator supplies the sequence. The firmware builds each URL as
`<release-base>/vN/roadhog-vN.<suffix>`. The standard base is
`https://github.com/sshorba/roadhog/releases/download`. This avoids GitHub API
queries and device tokens. Forks or bench servers may override the base and
exact case-sensitive hostname allowlist. Bench fixtures can use a restricted
network endpoint with a certificate in the firmware's trust bundle. Fetches
follow at most three absolute HTTPS redirects per asset, recheck the allowlist,
and verify the certificate and hostname/SNI on every hop. HTTPS uses port 443.
There is no automatic discovery. A future latest-release pointer can identify a
candidate sequence, which still needs signed-manifest verification.

## Trust and rollback

Manifest signing authenticates the content even if release hosting changes or is
compromised. This is application-level authentication. The policy compares the
signed sequence to the running application's decimal project version. It does
not burn eFuses or record a durable high-water mark, so ESP-IDF can automatically
return to a previously validated image. After a rollback, a release newer than
the running image can be explicitly attempted again. Do not reuse a release
sequence for different content. A known failed release should be withdrawn.

For planned rotation, publish a transition image with the new public key while
the current key is still trusted, sign that release with the current key, and
finish validation before serving new-key releases to those devices. A rollback
to an older image also restores its older trust anchor. Maintain a tested
recovery image. If a trusted private key is compromised, stop network updates,
withdraw affected releases, and locally reprovision a known-good image and
replacement public key under owner approval. An update signed only by a
compromised key cannot establish a trustworthy rotation. This does not
configure ESP32 Secure Boot.

## Bench procedure

1. Preserve the full recovery backup and private configuration. Confirm the
   current OTA slot/state, supply voltage and stable external power for both
   devices. Leave the inactive slot available. Build the normal and LTE/OTA
   versions locally with v6.1.1. For the installer, layer the LTE diagnostic and
   16 MB OTA defaults, preserve the private APN and SMS owner, and enable
   `CONFIG_ROADHOG_OTA_UPDATE_BENCH`. Disable both rollback bench flags.
2. Build the installer with `-D PROJECT_VER=1` and
   `-D ROADHOG_OTA_PUBLIC_KEY=/absolute/path/to/test-public.pem`. Build a
   genuinely newer bench candidate with `-D PROJECT_VER=2` and the same private
   settings. Check app size and image info. Serve the three signed assets on an
   approved private endpoint. Adjust the release base and host allowlist in the
   installer before its build when using a bench server. Use an ephemeral
   test key and do not reuse a production key.
3. After separate approval, provision the bench installer into the intended
   application slot using the existing layout. Verify that it is selected
   and VALID, and verify SMS STATUS, GPS and portal operation. Do not erase
   metadata or change other partitions as part of an OTA install.
4. Send one complete line `ota check 2` through the configured physical UART
   console. The explicit dry run makes no application writes. Confirm PPP,
   signed-manifest verification, command-mode recovery and SMS restoration.
   There is no scheduled LTE diagnostic in the enabled OTA bench build.
5. With stable power, send `ota install powered 2`. The operator confirmation
   is a bench prerequisite, since the device has no qualified power sensor.
   Capture the manifest, slot, length, digest and download progress. Confirm
   the device stages only the inactive application, verifies it, closes HTTPS,
   tears down PPP, and restores AT/SMS before selecting the new boot slot.
   GNSS is reinitialized after the AT driver reopens. SMS and GNSS AT operations
   pause while PPP owns the UART. The Wi-Fi portal stays up.
6. Verify SMS response after the download. Reboot the ESP32 manually only after
   the candidate is selected. Capture pending verification and the health
   result, and verify a subsequent boot stays on the validated image. Test SMS
   STATUS/GPS, the portal and LEDs. Check NVS and the working application slot
   against the prior snapshot where applicable. Use a separate approved
   procedure for a failing candidate or flash recovery.

The transfer checks PPP routing, uses bounded DNS waits, an HTTP I/O timeout,
a five-minute transfer budget capped at seven minutes from command initiation,
checked between SDK calls, and an eight-minute
outer modem-session deadline. A server that trickles bytes inside a blocking
ESP-IDF header/body call may exceed the transfer budget before that call
returns. The outer deadline fails closed and leaves the UART with its current
owner if recovery has not finished. Restart both devices in documented order
if the firmware cannot recover command mode. No timed-out operation can select
a boot slot. A failed write can leave partial bytes in the inactive app, which
the next attempt overwrites. ESP-IDF v6.1.1 sequential OTA resumption at
offset zero avoids the inactive metadata erase performed by esp_ota_begin().
Only the final ESP-IDF boot selection updates OTA metadata.

## Local verification

Run these commands with a C toolchain, Python 3, OpenSSL, and the libcurl
and OpenSSL development packages available.

```sh
sh tests/ota_release/run.sh
python3 tests/ota_release/run_integration.py /path/to/esp-idf-v6.1.1
sh tests/ota_boot_health/run.sh
python3 tests/ota_boot_health/run_sdk_lifecycle.py /path/to/esp-idf-v6.1.1
```

The integration runner binds a temporary localhost HTTPS server and generates
disposable TLS and release signing keys. It executes the OTA installer and LTE
recovery wrapper with host adapters for HTTP, crypto, modem and flash. It uses
real P-256 signatures, TLS hostname/CA checks and SNI via OpenSSL/libcurl, and
executes the pinned ESP-IDF `esp_ota_resume()` and `esp_ota_write()` functions on
simulated NOR flash. No live GitHub calls or device access are needed.

The fixture stages and selects a signed image in both slot directions over
GitHub-style redirects, including a chunked transfer. It starts with a populated
inactive slot and checks the erased sectors, image bytes, and untouched tail.
It checks that a dry run, invalid signature, altered or truncated image,
HTTP failure, invalid staged image, readback mismatch, TLS or redirect failure,
unavailable slot, lost data link, or failed LTE/AT/SMS/GNSS recovery cannot
select a candidate. All cases compare the complete flash outside the inactive
application byte for byte, including the running image and NVS. Boot selection
is modeled separately, and the SDK lifecycle fixture exercises the real
boot-selection and rollback logic.

## Remaining hardware validation

Use only a private, signed test release with the existing compiled SMS owner
for the first trial. After separate approval:

1. Confirm the currently selected VALID image, stable bench power and usable
   SMS, GPS and portal. Capture partition and NVS baselines as needed.
2. Run the dry run over SIM7600 PPP using the ESP-IDF certificate bundle.
   Exercise a hostname mismatch, untrusted CA and a private endpoint that
   returns an allowlisted HTTPS redirect. Confirm TLS failures abort and that
   redirects keep the intended TLS peer name and Host header.
3. Install a valid candidate to the already populated inactive slot. Verify
   the image hash, contents of the running slot, and boot metadata. Confirm
   successful LTE teardown and working SMS and GPS before manually rebooting.
4. With separate authorization as needed, exercise a truncated or tampered
   private transfer, link loss and a modem recovery failure. Confirm the
   running slot stays selected. Check NVS byte for byte in trials without
   intentional configuration changes and check the existing SMS permissions.
5. Verify ESP-IDF image validation, pending verification and a persistent
   VALID boot on the prototype. Test rollback with an approved rejecting
   candidate. A controlled brownout test needs a separate procedure and
   approval. Live GitHub CDN certificate and redirect behavior also remains
   unverified and is outside the first private installation trial.
