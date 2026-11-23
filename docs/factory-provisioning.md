# Factory provisioning and prototype reset review

Issue #11 is independent of the GitHub Releases installer. This draft
implements device state, credential storage, protected portal, wizard, SMS
gating and read-only SIM number display. Credential handoff and the destructive
prototype reset procedure are pending review.

## Boot and claim state

| State | Expected behavior |
| --- | --- |
| Factory | The application settings and prior ownership namespaces are absent. Start WPA2 AP with a per-boot random Wi-Fi passphrase, allow one station, and leave it up until a provisioned reboot. SMS is disabled. |
| Configured | The versioned settings blob parses and validates. Use the stored owner, APN, administrator hash and Wi-Fi credential. Switch to the configured AP lifecycle. Load additional users from the existing SMS namespace. |
| Recovery required | NVS failed, the settings namespace is present without a valid record, or legacy ownership data exists without settings. Do not open a claimable AP or accept SMS. Log only the setup/validation failure. |

The wizard accepts an owner number, APN and an administrator password in one
authorized request. The server requires the setup authorization code over the
private AP, compares it in constant time, validates bounded input, and
consumes the code when it attempts a write. Owner and administrator are
separate identities. A single versioned blob at `device_cfg/settings_v1`
stores the E.164 SMS owner, APN, Wi-Fi credential and salted
PBKDF2-HMAC-SHA-256 administrator hash. The server commits and checks readback.
It never activates a partially written record from RAM. After reboot a valid
record enables the configured behavior. If a power interruption leaves an
unreadable or incomplete record, a local recovery review is required. A
failure after a durable commit can still yield a configured device after
reboot. Repeat submissions cannot replace an owner.

The normal password change uses the same record and preserves its owner,
APN and Wi-Fi credential. No default owner or administrator password is
embedded. Existing additional-user permissions, including owner-only user/AP
management, are retained. Legacy ownership namespaces block automatic claiming
without altering their contents. The draft introduces no bridge migration.
The current prototype's configuration and backups are private. Password
hashing reduces exposure from NVS readback but owner, APN and AP passphrase
are recoverable from plaintext NVS on a device without flash/NVS encryption.
Neither an application reset nor this wizard changes eFuses or enables secure
boot, flash encryption or NVS encryption.

## Physical authorization to review

The build generates independent 128-bit Wi-Fi and setup secrets with ESP32-S3
hardware entropy before the radio starts. It has no production route to reveal
those secrets. The indefinite AP and token-gated wizard can be tested with
controlled fixtures. An approved local handoff is required before an owner
can join and claim on hardware.

For the bench, consider a deliberate local action after firmware has booted,
followed by a short-lived credential handoff through supervised wired access.
Prefer a dedicated non-strapping input for production. The DevKitC-1 BOOT
button is wired for download mode. GPIO0 held at reset, and USB bridge
DTR/RTS activity, can affect boot selection. A BOOT-based bench proposal needs
actual board revision, wiring, debounce, reset and serial bridge validation
before approval. Do not make an ordinary reboot or a held-at-startup pin
authorize setup. Avoid routine console logging of tokens or passphrases.
A protected fixture or another deliberate local interface could deliver
credentials to the physically present owner. Decide the handoff, expiration,
and whether to require application-level TLS before production use. The draft
uses HTTP inside WPA2 link protection with AP address, Host and Origin checks.
Another party trusted with the AP credential is inside that trust boundary.

[Espressif board guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html) · [ESP32-S3 boot mode selection](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/advanced-topics/boot-mode-selection.html)

## SIM number and OTA behavior

Read the SIM number with the SIM7600 execution command `AT+CNUM` on the
normal AT UART after startup probes and before PPP. Cache it without logging
the value. The parser accepts a single unambiguous quoted digits-only
national number or a validated international number. Ambiguous multiple
entries, unsupported responses, malformed or absent values show
“Not available from SIM/carrier.” This field is independent of LTE service
and the owner number. The dashboard requires administrator authentication.
The wizard requires its setup authorization code before reading the cached
value. The GUI renders it as text.

[SIMCom SIM7500/SIM7600 AT Command Manual, section 6.2.5](https://www.waveshare.net/w/upload/6/68/SIM7500_SIM7600_Series_AT_Command_Manual_V2.00.pdf)

Healthy factory boot can validate an OTA image without any owner. It still
requires the portal AP, modem AT and GNSS startup checks. Configured boot
continues to require SMS startup when enabled. An unreadable provisioning state
fails the normal portal health check. Preserve the current A/B layout and
metadata through normal use. When integrating Issue #9, read the APN from
the same provisioning API and keep its single UART owner and rollback gates.

## Prototype reset proposal for security review

The most recent readback showed a selected VALID slot A at `0x10000` and a
rejected slot B at `0x410000`. Both hold personalized build artifacts.
Recheck the device and state before any future operation. An older image can
contain an owner despite erased NVS, and metadata changes or bootloader
fallback may run it. Clearing user settings alone therefore cannot establish a
safe generic factory device. Retaining an INVALID entry for the old image also
leaves the compiled owner bytes on flash.

1. With owner approval, verify a fresh full 16 MiB backup, individual OTA and
   NVS images, metadata and security settings against the physical device.
   Keep current and original backups outside Git. Keep the modem or SIM
   disconnected from service until it is safe to boot the generic code.
2. Review replacing both 4 MiB app slots with verified generic images, or
   fully erase the nonbooting personalized slot if only one generic image
   is needed. Plan full slot erasure before writing new images if residual
   owner bytes must be removed. Keep the device in ROM mode until neither
   bootable nor residual personalized application remains. Preserve the
   bootloader, partition table, PHY and other partitions. Review OTA
   metadata separately and verify boot selection without an old
   personalized fallback. The exact slot and metadata operations need
   independent owner approval.
3. For a genuinely new-device test, the intended NVS scope is the application
   settings entry and namespace `device_cfg/settings_v1`, legacy portal
   credential namespace `portal_auth` (key `password_v1`), and legacy SMS
   namespace `sms` (key `users_v1`, including any additional-user grants).
   Because the factory gate checks namespace presence, deleting values alone
   can leave a namespace marker. Confirm the actual NVS inventory and review a
   prepared replacement NVS image that preserves reviewed unrelated records
   and omits these namespaces, or approve erasing only the NVS partition if
   the inventory proves no other needed data exists. A reset must never
   silently carry old additional-user grants to a new owner. An ordinary
   firmware update or reboot preserves `users_v1`.
4. Boot only after verified readback and the authorized claim mechanism are
   ready. Verify persistent protected AP, lack of SMS authorization, rejected
   unauthenticated claims and SIM reads, successful local setup, power-cycle
   persistence, normal AP timeout, owner-only SMS permissions, and rollback
   to a generic image. For recovery, restore the verified matching firmware,
   configuration and OTA metadata only under owner approval.

No reset endpoint or destructive reset command is included in the draft.

## Local build and tests

```sh
idf.py -B build/factory -D SDKCONFIG=build/factory-sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.ota-16mb.defaults' build
sh tests/provisioning/run.sh
sh tests/ota_boot_health/run.sh
python3 tests/ota_boot_health/run_sdk_lifecycle.py /path/to/esp-idf-v6.1.1
```

The host fixtures exercise factory, configured, legacy and corrupt
classification, authorization and repeat claims, SMS sender and owner-only
checks, write failures, interrupted states, reboot and SIM parsing. Physical authorization, AP lifetime on the
radio, actual NVS power-loss behavior and hardware OTA rollback require
bench validation after review.
