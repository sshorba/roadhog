# Factory provisioning and prototype reset review

Issue #11 is independent of the GitHub Releases installer. This draft
implements device state, credential storage, protected portal, wizard, SMS
gating and read-only SIM number display. The external setup button authorizes the final claim. The destructive
prototype reset procedure remains pending review.

## Boot and claim state

| State | Expected behavior |
| --- | --- |
| Factory | The application settings and prior ownership namespaces are absent. Start the WPA2 AP with the factory password `012345678`, allow one station, and leave it up until a provisioned reboot. SMS is disabled. |
| Configured | The versioned settings blob parses and validates. Use the stored owner, APN, administrator hash and Wi-Fi credential. Switch to the configured AP lifecycle. Load additional users from the existing SMS namespace. |
| Recovery required | NVS failed, the settings namespace is present without a valid record, or legacy ownership data exists without settings. Do not open a claimable AP or accept SMS. Log only the setup/validation failure. |

Join the factory AP and select **Start setup**. One browser setup session
can request an SMS for the proposed E.164 number and submit the six-digit
code from the message. The SIM number
display is independent of the proposed destination.

- The normal AT loop sends a GSM 7-bit PDU to the pending number after LTE
  registration. The HTTP task queues requests and reads status. It never
  controls the UART. Modem acceptance only means it accepted submission.
- The firmware generates the challenge with the ESP32 RNG while Wi-Fi is
  active. It is bound to the recipient and the active browser setup session.
  A queued request expires after 90 seconds. A successfully submitted code
  expires after five minutes. Verification allows five failures per code and
  at most one guess per second. The code, destination and SMS payload stay
  out of serial logs and status endpoints.
- Resend or changing the destination is limited globally to one request per
  minute and five per rolling 15 minutes. A successful resend replaces the
  previous challenge. Change number and Retry later cancel it without
  resetting the send limits. A cancellation can race with an in-progress
  SMS, whose late result cannot restore the challenge.
- If SMS submission fails, check registration, service allowance and the
  destination, then retry after the cooldown. An undelivered SMS can be
  resent even after modem acceptance. If the modem's AT stream is faulted,
  restart the modem and ESP32 in the documented order. A restart discards
  pending verification and resets the in-memory limits. Start a new
  browser setup session after restart. The AP stays up throughout setup.

The proposed owner and administrator remain unprivileged during verification.
Valid SMS verification freezes the owner, APN and administrator hash for the
pending session and opens a 30-second physical confirmation window. The
browser prompts the user to press the button, then requests the commit using
its original session token. Only then does the server commit and read back
`device_cfg/settings_v1`. The record includes a randomly generated configured
Wi-Fi password, displayed to that browser after a successful write. Save it
before restarting. The device keeps the factory AP until reboot, when the
saved credentials and owner take effect. An uncertain write locks the session
and requires a reboot. Repeat completion requests may retrieve the configured
Wi-Fi password without repeating the write.

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

## External setup button

Wire a normally open momentary switch between **GPIO4** and **GND**. GPIO4
uses an internal pull-up and reads low while pressed. The DevKitC-1 v1.1
exposes GPIO4 on J1 pin 4. The modem UART uses GPIO17/18 and the board LED
uses GPIO38 or GPIO48, depending on revision. The firmware rejects a build
that assigns the status LED to GPIO4. Confirm the prototype's physical wiring
before powering or testing the switch. [Espressif board guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html#header-block)

The input is configured only on a factory boot. After SMS verification,
the sampler requires 40 ms of stable release followed by 40 ms of stable
press before the deadline. A press before the window, a button held when the
window opens, contact bounce and a late press cannot authorize setup. A
configured device has no button-driven erase or reset behavior.

The setup session uses an unpredictable token with a 15-minute lifetime.
Arming the button gives the same browser a fresh session interval and the
full 30-second button window. Cancel or reboot discards the pending settings.
A lost browser session can be restarted once it expires, or by rebooting an
unprovisioned device. A fresh button press is required for the replacement
session after it verifies SMS. Only the browser that submitted the verified
settings can finish the pending claim or read its result.

The factory password is deliberately shared. The setup portal uses HTTP
over that Wi-Fi and checks the local address, Host and Origin. The shared
password does not authenticate the owner or protect against another party
that knows it and can capture or interfere with local traffic. Run initial
setup in a controlled location and press the button only when your own
browser prompts you. Review application-level transport protection before
relying on setup around untrusted parties. Configured devices use the
generated Wi-Fi password and existing administrator login and AP timeout.

## SIM number and OTA behavior

Read the SIM number with the SIM7600 execution command `AT+CNUM` on the
normal AT UART after startup probes and before PPP. Cache it without logging
the value. The parser accepts a single unambiguous quoted digits-only
national number or a validated international number. Ambiguous multiple
entries, unsupported responses, malformed or absent values show
“Not available from SIM/carrier.” This field is independent of LTE service
and the owner number. The dashboard requires administrator authentication.
The wizard shows the cached value only in the successfully confirmed
setup session. The GUI renders it as text.

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
4. Boot only after verified readback and the external button and its wiring have been reviewed. Verify persistent protected AP, lack of SMS authorization, rejected
   unauthenticated claims and SIM reads, SMS verification followed by the fresh GPIO4 press, power-cycle
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
checks, SMS PDU encoding and modem failure handling, resend and guess limits, expiration, replay, write failures, interrupted states, reboot and SIM parsing. The button fixture covers a held input, contact bounce, pre-window and late presses,
expiry, cancellation, session mismatch and single-write completion. Verify actual
GPIO wiring, radio behavior, NVS power loss and hardware rollback on the bench.
