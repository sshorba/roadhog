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
publication. Build installer and release artifacts from reviewed source commits
and record their build configuration and SHA-256 digests. The generic images
load the owner, carrier APN, portal settings and admin credentials from
`device_cfg/settings_v1` in NVS, via [factory provisioning](factory-provisioning.md).
Neither the release nor the signing manifest contains per-device provisioning.
Verify this before approving any public asset.

For a future owner-approved publication, create a GitHub release for tag
`vN` at the exact reviewed source commit. Attach the three verified assets
to the draft and review the release notes, tag, target, sequence and digests.
Only publish after bench acceptance. A draft or private GitHub release is
unavailable to the firmware's unauthenticated HTTPS fetcher. Use the
separate bench HTTPS endpoint below for prepublication trials.

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

1. In an owner-approved maintenance window, confirm stable power for the
   modem and ESP32, a verified private recovery backup, the expected 16 MiB
   A/B layout and a selected VALID application. Check STATUS, GPS and portal
   access on the existing generic-provisioned firmware. Use the existing local
   Ubuntu station for passive reports and, if separately approved, a read-only
   layout check. Keep the recovery backup offline.
2. Build the generic installer and newer candidate locally from reviewed source
   using ESP-IDF v6.1.1. Layer the 16 MiB OTA and LTE diagnostic defaults,
   enable `CONFIG_ROADHOG_OTA_UPDATE_BENCH` for the installer, and leave both
   rollback bench flags disabled. Use decimal project versions such as 1 and 2.
   Embed the ephemeral bench public key with
   `-D ROADHOG_OTA_PUBLIC_KEY=/absolute/path/to/test-public.pem` for each
   OTA-capable image. Check image metadata and slot size. Sign the three
   candidate assets and serve them over approved private HTTPS. Set the bench
   release base and allowlist before building the installer.
3. If the selected app has no OTA console, provision the installer only through
   a separately approved local maintenance procedure. Verify the selected
   VALID installer and repeat the SMS, GNSS and portal baseline. Preserve
   the existing provisioning, bootloader, partition table and recovery image.
4. Pause the station's passive serial reader while the operator owns the port.
   Confirm the attached bridge exposes the configured ESP32 UART console.
   Send `ota check 2` as a complete console line. Confirm PPP, signature
   verification and AT/SMS/GNSS recovery. The dry run leaves the app slots
   unchanged. The installer has no scheduled PPP diagnostic.
5. After the owner approves the exact signed candidate and confirms stable
   power, send `ota install powered 2` locally. Verify the inactive slot,
   reported size and digest, and restored AT/SMS/GNSS before checking the new
   boot selection. SMS service and GNSS AT queries pause while PPP holds
   the UART. The Wi-Fi portal stays up.
6. Reboot manually only after a separate owner decision. Release the local
   console before resuming passive capture with the station. Verify the
   candidate moves from PENDING_VERIFY to VALID, remains selected after a
   later approved reboot, preserves provisioning, and passes STATUS/GPS
   and portal checks. If the candidate fails, verify the prior VALID slot
   returns and follow the separate recovery procedure if needed.

The Phase 1 hardware agent remains read-only. The operator handles any
console input, installer provisioning, and resets locally.

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
unavailable slot, factory/no-APN state, lost data link, or failed LTE/AT/SMS/GNSS recovery cannot
select a candidate. All cases compare the complete flash outside the inactive
application byte for byte, including the running image and NVS. Boot selection
is modeled separately, and the SDK lifecycle fixture exercises the real
boot-selection and rollback logic.

## Remaining hardware validation

Use a private, signed test candidate on the existing generic-provisioned
prototype. After separate approval:

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
