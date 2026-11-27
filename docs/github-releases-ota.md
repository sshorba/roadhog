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
The serial flow uses an operator-supplied sequence. The optional owner SMS
flow discovers a signed HogBarn pointer on request.

## Trust and rollback

**Experimental, default-off bench feature.** With Secure Boot disabled, ESP-IDF
bootloader fallback can consider the inactive slot if it cannot load the
selected image. The inactive slot can contain a complete unauthenticated or
partially written image from an interrupted or rejected transfer, even
though the application never selected it. Signed manifest and readback
verification guard normal boot selection. They do not enforce authenticity
in bootloader fallback. Keep OTA disabled for unattended deployments, fielded
vehicles and assembled units until the fallback trust boundary is protected
and hardware power-loss tests pass.

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

Start with the [attended preflight](lte-ota-preflight.md) to identify the current
image and decide whether installer provisioning is needed. The dry run,
installation and first normal boot each require owner approval.

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
   boot selection. Once selected, the candidate will boot on the next reset,
   including an unexpected power cycle. SMS service and GNSS AT queries pause
   while PPP holds the UART. The Wi-Fi portal stays up.
6. Rehearse the console-to-ROM handoff while the old slot is still selected.
   Before allowing a normal boot of the candidate, enter ROM directly without
   releasing the serial connection or permitting an unintended normal reset.
   Read the exact signed image length from the newly selected slot and compare
   its SHA-256 with the manifest. Check that the new OTA record is NEW, the
   previous VALID record and full fallback slot are intact, and the protected
   regions and provisioning records match. Stop on any discrepancy.
7. After the readback gates pass, allow one controlled normal boot. Verify
   the candidate goes from PENDING_VERIFY to VALID and that provisioning,
   modem AT, SMS startup, GNSS, and the console resume. If the candidate
   fails or rolls back on its own, capture the evidence and stop for review.
   Manual recovery requires separate approval.

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

## Modem ownership and bench console

The OTA session owns the modem UART from PPP setup through AT recovery.
Its dedicated 24 KiB worker logs the minimum free stack in bytes. The plain
LTE diagnostic keeps its original stack budget. After an ESP32 restart,
startup drains late input after a failed probe and retries with a different
read-only query. If the first final is still outstanding, startup requires
another distinct typed confirmation before releasing the UART to normal
traffic. Generic finals on the UART carry no transaction identity, so no
timing window or flush can prove which command produced an OK. The extra
typed confirmation corroborates command mode under normal ordered replies.
When probes leave the result ambiguous, startup skips PPP escape and hangup.
An ESP32 restart with a modem still in silent PPP can therefore require
attended recovery. A pending image still requires the full startup health
checks before it can become VALID. PDP cleanup is checked separately from
AT mode so SMS and GNSS can resume when command mode is confirmed.

The firmware journals intent before defining a confirmed-free diagnostic
CID 8. The record is tied to the modem identity and provisioned APN. Only
an inactive compatible context with matching ownership can be reused after
a crash. Active, incompatible, secondary-dependent, or unowned contexts
are preserved. Active CID 8 left by a crash can block a later diagnostic
even with a matching journal. Recovery for that case is deferred and may
require attended modem maintenance or reboot. The journal is cleared after
verified cleanup. Physical NVS
bytes can change during journaling. Compare live provisioning records and
verify that no temporary journal entry remains.

Both `CONFIG_ROADHOG_OTA_UPDATE_BENCH` and
`CONFIG_ROADHOG_MODEM_MAINTENANCE_BENCH` default to off. The optional
maintenance console requires the OTA bench option and a 6 KiB main-task
stack. Use `modem inspect` for a sanitized inventory. If the owner separately
approves removal of a verified inactive legacy CID 8, issue
`modem cleanup 8` to arm cleanup and enter the one-time confirmation locally.
The console rechecks the modem identity, exact profile, activity, secondary
dependencies and journal state before issuing the targeted deletion.
Keep the token and raw logs local. Never treat an unexplained CID 8 as owned.
The console provides no general AT passthrough and is unavailable over SMS
or the portal.

## Local verification

Run these commands with C and C++ toolchains, Python 3, OpenSSL, the locked
esp_modem component, and libcurl and OpenSSL development packages available.

```sh
sh tests/ota_release/run.sh
python3 tests/ota_release/run_integration.py /path/to/esp-idf-v6.1.1
sh components/cellular_net/tests/run_cid8.sh
sh components/cellular_net/tests/run_dns.sh
sh tests/modem/run.sh
sh tests/ota_boot_health/run.sh
python3 tests/ota_boot_health/run_sdk_lifecycle.py /path/to/esp-idf-v6.1.1
sh tests/provisioning/run.sh
sh tests/portal/run.sh
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
boot-selection and rollback logic. The installer tests inject an erase
failure, a partial app write, and a boot-selection error. They check
transaction cleanup, modem restoration, reporting, and unchanged bytes
outside the inactive application. The SDK metadata tests verify that the
running VALID record survives an erase failure and a torn peer record with
no valid CRC. The peer sector may change. These cases do not establish
behavior for every flash fault or power-loss point.

## Observed bench results

- The private signed-manifest dry run established PPP, retrieved and verified
  the manifest and detached signature over authenticated HTTPS, and restored
  AT/SMS/GNSS without staging an app. It recorded 18,864 bytes of minimum OTA
  worker stack. [Dry-run report](https://github.com/sshorba/roadhog/pull/10#issuecomment-7170933525)
- One `ota install powered 2` transferred a 1,078,528-byte signed image from A
  to B and logged 17,324 bytes of minimum free stack. A bare-ROM read of the
  exact signed length at `0x410000` matched SHA-256 before B was allowed to
  boot. The OTA record selected B as NEW and preserved VALID A. The app,
  protected regions and provisioning records were checked. One controlled
  normal boot confirmed v2 moved from PENDING_VERIFY to VALID with modem
  registration, SMS startup, GNSS and console readiness.
  [Install report](https://github.com/sshorba/roadhog/pull/10#issuecomment-7171430168)

The installed v2 candidate was built from an earlier revision. It predates
the 24 KiB OTA worker, startup modem recovery, CID ownership journal and
maintenance console used by the v1 installer. Its successful boot does not
validate those improvements in v2.

## Remaining hardware and release validation

- Schedule a separate attended trial with a signed, deliberately rejecting
  candidate and a verified recovery backup. Verify the bootloader returns to
  the old VALID slot, and confirm provisioning and modem health afterward.
  Forced rollback from the signed LTE path is covered offline and remains
  untested on hardware.
- Exercise rejected TLS peer names, allowlisted redirects with per-hop server
  SNI/Host evidence, and interrupted transfers on the ESP32. The successful
  private endpoint trial did not capture per-hop SNI or exercise the GitHub CDN.
  Brownout and repeated-boot validation require separate approval.
- Treat local authorization, trust-anchor lifecycle, distribution, and tested
  recovery as release gates. The bench console and disposable bench key are
  development tools. A passing private trial alone does not qualify a public
  firmware release.

## Owner-confirmed SMS OTA on HogBarn

Set `CONFIG_ROADHOG_OTA_SMS_BENCH=y` in the attended installer build in addition
to the serial OTA bench settings above. Set `CONFIG_ROADHOG_OTA_SMS_RELEASE_BASE`
to the HogBarn HTTPS release directory, including a path and with no trailing
slash or query string. Add the exact HogBarn hostname and any approved redirect
hosts to `CONFIG_ROADHOG_OTA_RELEASE_HOSTS`. This leaves the serial release base
independent. The SMS command path defaults off. A release built with rollback
and SMS support from this source can report a pending result even with the
SMS OTA command option off.

Publish the same three signed assets under `<base>/vN/`. Use immutable,
increasing sequence numbers. Once all three objects are readable at their
final URLs and the detached signature has been verified, build a single
`latest` object from the exact versioned manifest and signature. The object
contains one unsigned byte with the DER signature length (1–72), the DER
signature, and the canonical signed manifest (1–1024 bytes). For example,
given the verified release assets in the current directory:

```sh
python3 - roadhog-v2.manifest roadhog-v2.sig <<'PY'
from pathlib import Path
import sys

manifest, signature = (Path(name).read_bytes() for name in sys.argv[1:])
assert 0 < len(manifest) <= 1024 and 0 < len(signature) <= 72
Path("latest.new").write_bytes(bytes([len(signature)]) + signature + manifest)
PY
```

Atomically replace `<base>/latest` using a single object PUT or a rename on the
same filesystem. Configure the server and CDN to revalidate the mutable object
on each request. Readers can see a complete previous or new pointer during
publication. A stale authentic pointer can still offer a release newer than the
device. TLS and the signature authenticate content, while freshness depends
on publication and caching. Future HogForge publishers can use the same assets
and publish the final `latest` object after promoting a reviewed release.
Withdrawing the pointer stops new offers. To withdraw a specific offered release,
also make its versioned manifest or image unavailable, since an existing
challenge binds the versioned release for up to five minutes.

Only the provisioned owner can use `OTA CHECK`, `OTA CONFIRM <code>`,
`OTA CANCEL`, and `OTA STATUS`. The modem supplies the originating
phone number. Spoofing and SIM or handset compromise remain risks.
`OTA CHECK` fetches signed metadata over LTE and leaves both app slots
untouched. The response includes the current and candidate sequences and a
12-character hexadecimal code. The code expires five minutes after issuance
on the device's monotonic clock and is discarded on reboot. A fresh check
replaces the previous offer. Confirming consumes the code before installation
is queued. A failed acknowledgement or reply prevents the queued network
operation. Cancellation clears the pending offer or a request still queued.
During PPP, SMS reception and GNSS AT queries pause. Cancellation cannot
interrupt an active PPP transfer. The versioned signed manifest must match
the confirmed sequence, target, size and SHA-256 before any image is staged.

The installer commits a pending report in its own NVS namespace before boot
selection. On reconnect, the boot reporter requires the selected, running
partition to be VALID and matches the version, slot and exact image digest
before claiming success. It waits for LTE registration, SMS configuration and
GNSS service. The message reports LTE registration and SMS readiness. It does
not perform a second PPP or HTTPS test. If the previous firmware is running
again, the outcome may be rollback or an interruption before selection.
Without communication the outcome stays unknown. The record is consumed
before SMS submission to prevent duplicate submissions across reboots.
Power loss in that interval, ambiguous modem submission, or carrier delivery
failure can therefore lose the report. The local console is still available
for attended diagnostics.

### Attended SMS bench procedure

Follow the [preflight](lte-ota-preflight.md) and obtain separate hardware
approval before each trial. Keep the approved local recovery path available.

1. Start on the selected, VALID installer with the expected layout, stable
   ESP32 and modem power, the verified recovery backup, and an owner handset.
   Check SMS, LTE registration, GNSS, and portal access. Keep any challenge
   and handset details out of shared logs.
2. Publish a newer signed fixture on the approved HogBarn bench endpoint.
   From an additional authorized SMS number, try all four OTA commands and
   verify they cause no response or update. From the owner handset send
   `OTA STATUS`, then `OTA CHECK`. Verify the response sequences and that
   the endpoint served only the pointer. Check that the inactive slot and
   boot selection are unchanged using the separately approved readback path.
3. Send `OTA CANCEL`, then confirm with the captured code. Check rejection
   and no versioned asset requests. Repeat with a new code held for more
   than five minutes. Exercise missing, invalid, equal, and older pointers.
4. Send `OTA CHECK` again and confirm the fresh code during the maintenance
   window. Check that the confirmation reply precedes versioned requests.
   Confirm the automatic reboot follows staged image validation and modem
   recovery. Verify the candidate reaches VALID, the success SMS arrives
   once, and provisioning, SMS, GNSS, the portal, and local diagnostics work.
   Replay the old confirmation and check that no update begins.
5. On separate trials, replace the versioned signed manifest after discovery
   and interrupt the network during download. Verify that the prior VALID
   slot remains selected and SMS recovers when command mode returns. For a
   forced rollback trial, use a separately signed candidate with the
   existing forced-reject bench option. Check the original slot returns
   as VALID and the owner receives an unsuccessful update report once
   communication returns. Record what happened on the modem and device.
