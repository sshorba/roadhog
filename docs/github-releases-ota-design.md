# OTA owner provisioning design for Issue #9

The [LTE OTA bench procedure](github-releases-ota.md) uses a private
personalized candidate before generic release provisioning is available.
## Proposed one-time SMS owner migration

Today startup validates `CONFIG_ROADHOG_SMS_OWNER` and loads the existing
additional-user blob from namespace `sms`, key `users_v1`. Incoming requests
must match the owner or an additional user. User-management commands compare
the sender directly to the owner. Preserve those semantics.

1. Deliver a personalized bridge build with the current compiled owner and
   unchanged SMS permissions. With the correct device and a verified backup,
   validate the compiled E.164 number, then initialize a separate `sms/owner_v1`
   key only if it is absent. Commit and read back the value before enabling
   SMS or allowing an update to a generic release. Preserve `users_v1`
   byte-for-byte. If there is an existing owner, validate it and require it
   to equal the compiled value during the one-time migration. Stop on conflict
   or storage error. Repeated boots are idempotent. A rollback to the legacy
   build still honors the same compiled owner and shared user list.
2. After bench verification, the generic release should require a valid
   stored owner before enabling any SMS processing, including for devices
   with valid additional users. Missing, malformed or unreadable owner
   storage leaves SMS control disabled. Record an actionable local error
   without logging the number. An incoming SMS can never claim an owner.
3. For newly unprovisioned devices, use a separately reviewed physically
   authorized local provisioning procedure. Reassignment and recovery also
   require physical authorization. Never silently erase NVS, promote an
   additional user, or fall back to a blank/default owner. Verify owner-only
   commands, the unchanged additional-user allowlist and rollback both before
   and after installing a generic image.

This migration is a proposal. The bench installer uses the existing SMS
authentication code without any owner-storage changes.
